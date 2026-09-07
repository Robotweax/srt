from __future__ import annotations

import argparse
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


BENCHMARKS_DIRECTORY = Path(__file__).resolve().parents[2] / "benchmarks"
sys.path.insert(0, str(BENCHMARKS_DIRECTORY))

import scalability_scorecard  # noqa: E402


class ScalabilityScorecardTests(unittest.TestCase):
    def test_topology_defaults_reserve_many_socket_playout_headroom(
        self,
    ) -> None:
        common = [
            "--robotweax-peer",
            "/tmp/peer",
            "--profile",
            "robotweax-self",
            "--output",
            "/tmp/report.json",
        ]
        isolated = scalability_scorecard.parse_arguments(common)
        many_socket = scalability_scorecard.parse_arguments(
            ["--topology", "many-socket", *common]
        )
        self.assertEqual(isolated.latency_ms, 20)
        self.assertEqual(isolated.shutdown_grace_ms, 50)
        self.assertEqual(many_socket.latency_ms, 120)
        self.assertEqual(many_socket.shutdown_grace_ms, 500)

    def test_positive_list_is_bounded_and_deduplicated(self) -> None:
        self.assertEqual(
            scalability_scorecard.parse_positive_list(
                "1,8,8,32", maximum=64
            ),
            [1, 8, 32],
        )
        for value in ("", "0", "65", "one"):
            with self.subTest(value=value), self.assertRaises(
                argparse.ArgumentTypeError
            ):
                scalability_scorecard.parse_positive_list(
                    value, maximum=64
                )

    def test_distribution_uses_interpolated_percentiles(self) -> None:
        result = scalability_scorecard.distribution([1.0, 2.0, 3.0, 4.0])
        self.assertEqual(result["minimum"], 1.0)
        self.assertEqual(result["p50"], 2.5)
        self.assertAlmostEqual(result["p99"], 3.97)
        self.assertAlmostEqual(result["p99_9"], 3.997)
        self.assertEqual(result["maximum"], 4.0)

    def test_linux_status_parser_uses_bytes_and_thread_count(self) -> None:
        self.assertEqual(
            scalability_scorecard.parse_linux_status(
                "Name:\tpeer\nVmRSS:\t 1234 kB\nThreads:\t7\n"
            ),
            (1234 * 1024, 7),
        )

    def test_peer_command_fixes_comparable_public_api_profile(self) -> None:
        options = scalability_scorecard.RunOptions(
            host="127.0.0.1",
            connections=8,
            bytes_per_connection=4_194_304,
            message_size=1_316,
            timeout_seconds=60,
            latency_milliseconds=20,
            shutdown_grace_milliseconds=50,
            sampling_interval_seconds=0.01,
        )
        command = scalability_scorecard.peer_command(
            Path("/tmp/peer"),
            "caller",
            9000,
            Path("/tmp/payload"),
            options,
        )
        self.assertEqual(command[1], "caller")
        self.assertEqual(
            command[command.index("--chunk-size") + 1], "1316"
        )
        self.assertEqual(
            command[command.index("--payload-size") + 1], "1316"
        )
        self.assertEqual(command[command.index("--max-bw") + 1], "-1")
        self.assertIn("--input", command)
        self.assertNotIn("--output", command)

    def test_many_socket_command_uses_one_peer_for_all_connections(self) -> None:
        options = scalability_scorecard.RunOptions(
            host="127.0.0.1",
            connections=32,
            bytes_per_connection=4_194_304,
            message_size=1_316,
            timeout_seconds=60,
            latency_milliseconds=20,
            shutdown_grace_milliseconds=50,
            sampling_interval_seconds=0.01,
        )
        command = scalability_scorecard.many_socket_peer_command(
            Path("/tmp/many-peer"), "listener", 9000, options
        )
        self.assertEqual(command[1], "listener")
        self.assertEqual(
            command[command.index("--connections") + 1], "32"
        )
        self.assertEqual(
            command[command.index("--messages") + 1], "3188"
        )
        self.assertEqual(
            command[command.index("--message-size") + 1], "1316"
        )

    def test_summary_excludes_warmup_and_keeps_profiles_separate(self) -> None:
        def run(
            profile: str,
            throughput: float,
            *,
            warmup: bool = False,
        ) -> dict[str, object]:
            return {
                "profile": profile,
                "connections": 8,
                "message_size_bytes": 1_316,
                "warmup": warmup,
                "rates": {
                    "useful_bits_per_second": throughput,
                    "useful_messages_per_second": throughput / 10.0,
                    "cpu_seconds_per_useful_gigabit": throughput / 100.0,
                },
                "timing": {"transfer_seconds": 1.0},
                "process_resources": {
                    "peak_aggregate_rss_bytes": 1000,
                    "peak_aggregate_threads": 12,
                },
            }

        summaries = scalability_scorecard.summarize(
            [
                run("robotweax-self", 999.0, warmup=True),
                run("robotweax-self", 100.0),
                run("robotweax-self", 300.0),
                run("haivision-self", 50.0),
            ]
        )
        self.assertEqual(len(summaries), 2)
        robotweax = next(
            item
            for item in summaries
            if item["profile"] == "robotweax-self"
        )
        self.assertEqual(robotweax["iterations"], 2)
        self.assertEqual(
            robotweax["median_useful_bits_per_second"], 200.0
        )

    def test_robotweax_only_profile_does_not_require_reference(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            peer = Path(temporary) / "peer"
            peer.write_bytes(b"peer")
            output = Path(temporary) / "scorecard.json"
            arguments = scalability_scorecard.parse_arguments(
                [
                    "--robotweax-peer",
                    str(peer),
                    "--output",
                    str(output),
                    "--profile",
                    "robotweax-self",
                ]
            )
        self.assertEqual(arguments.profiles, ["robotweax-self"])
        self.assertIsNone(arguments.reference_peer)

    def test_profile_orchestration_validates_every_output(self) -> None:
        fake_peer_source = f"""#!{sys.executable}
import hashlib
import json
import sys
import time
from pathlib import Path

role = sys.argv[1]
arguments = dict(zip(sys.argv[2::2], sys.argv[3::2]))
size = int(arguments["--bytes"])
payload_argument = "--output" if role == "listener" else "--input"
caller_started = (
    Path(arguments[payload_argument]).parent
    / f"caller-{{arguments['--port']}}.started"
)
if role == "listener":
    print(json.dumps({{"event": "ready"}}), flush=True)
    remaining = size
    counter = 0
    with open(arguments["--output"], "wb") as output:
        while remaining:
            block = hashlib.sha256(
                b"robotweax-scalability-v1"
                + counter.to_bytes(8, byteorder="big")
            ).digest()
            selected = block[:remaining]
            output.write(selected)
            remaining -= len(selected)
            counter += 1
    while not caller_started.exists():
        time.sleep(0.005)
else:
    caller_started.touch()
time.sleep(0.02)
print(json.dumps({{
    "event": "complete",
    "role": role,
    "bytes": size,
    "elapsed_us": 1000,
    "srt_version": 66821,
    "stats": {{
        "pktSentUniqueTotal": 1,
        "pktSentTotal": 1,
        "pktRecvUniqueTotal": 1,
        "pktRetransTotal": 0,
    }},
}}), flush=True)
"""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            peer = directory / "fake-peer"
            peer.write_text(fake_peer_source, encoding="utf-8")
            peer.chmod(0o755)
            payload = directory / "payload.input"
            digest = scalability_scorecard.write_payload(payload, 4096)
            options = scalability_scorecard.RunOptions(
                host="127.0.0.1",
                connections=2,
                bytes_per_connection=4096,
                message_size=188,
                timeout_seconds=5,
                latency_milliseconds=20,
                shutdown_grace_milliseconds=0,
                sampling_interval_seconds=0.005,
            )
            with mock.patch.object(
                scalability_scorecard,
                "free_udp_port",
                side_effect=[9001, 9002],
            ):
                result = scalability_scorecard.run_profile(
                    "robotweax-self",
                    {"robotweax": peer},
                    options,
                    directory,
                    digest,
                    iteration=0,
                    warmup=False,
                )

        self.assertEqual(result["connections"], 2)
        self.assertEqual(result["integrity"]["verified_connections"], 2)
        self.assertGreater(result["rates"]["useful_bits_per_second"], 0)
        self.assertEqual(
            result["wire_statistics"]["retransmitted_packets"], 0
        )

    def test_many_socket_orchestration_validates_internal_integrity(
        self,
    ) -> None:
        fake_peer_source = f"""#!{sys.executable}
import json
import os
import sys
import tempfile
import time
from pathlib import Path

role = sys.argv[1]
arguments = dict(zip(sys.argv[2::2], sys.argv[3::2]))
connections = int(arguments["--connections"])
messages = int(arguments["--messages"])
message_size = int(arguments["--message-size"])
caller_started = Path(tempfile.gettempdir()) / (
    f"robotweax-scalability-{{os.getppid()}}-"
    f"{{arguments['--port']}}.started"
)
if role == "listener":
    caller_started.unlink(missing_ok=True)
    print(json.dumps({{"event": "ready"}}), flush=True)
    while not caller_started.exists():
        time.sleep(0.005)
else:
    caller_started.touch()
time.sleep(0.02)
print(json.dumps({{
    "event": "complete",
    "role": role,
    "connections": connections,
    "messages": connections * messages,
    "bytes": connections * messages * message_size,
    "elapsed_us": 2000,
    "establishment_elapsed_us": 1000,
    "srt_version": 66821,
    "integrity": True,
    "connection_completion_us": {{
        "minimum": 1000,
        "p50": 1500,
        "p99": 1990,
        "p99_9": 1999,
        "maximum": 2000,
    }},
    "stats": {{
        "pktSentUniqueTotal": messages,
        "pktSentTotal": messages,
        "pktRecvUniqueTotal": messages,
        "pktRetransTotal": 0,
    }},
}}), flush=True)
if role == "listener":
    caller_started.unlink(missing_ok=True)
"""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            peer = directory / "fake-many-peer"
            peer.write_text(fake_peer_source, encoding="utf-8")
            peer.chmod(0o755)
            options = scalability_scorecard.RunOptions(
                host="127.0.0.1",
                connections=3,
                bytes_per_connection=4096,
                message_size=188,
                timeout_seconds=5,
                latency_milliseconds=20,
                shutdown_grace_milliseconds=0,
                sampling_interval_seconds=0.005,
            )
            with mock.patch.object(
                scalability_scorecard, "free_udp_port", return_value=9001
            ):
                result = scalability_scorecard.run_many_socket_profile(
                    "robotweax-self",
                    {"robotweax": peer},
                    options,
                    directory,
                    iteration=0,
                    warmup=False,
                )

        self.assertEqual(result["connections"], 3)
        self.assertEqual(result["messages_per_connection"], 22)
        self.assertEqual(result["bytes_per_connection"], 4136)
        self.assertTrue(
            result["integrity"]["deterministic_payload_verified"]
        )
        self.assertEqual(
            result["timing"]["caller_completion_seconds"]["p50"],
            0.0015,
        )


if __name__ == "__main__":
    unittest.main()
