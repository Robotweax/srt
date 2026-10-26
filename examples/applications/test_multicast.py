#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Two public-API multicast receivers preserve UDP MPEG-TS and optional A/V."""
from __future__ import annotations

import argparse
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

from process_support import Process, ports


class Collector:
    def __init__(self, port: int, forward_port: int | None = None):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
        self.socket.bind(("127.0.0.1", port))
        self.socket.settimeout(0.1)
        self.stop = threading.Event()
        self.values: list[bytes] = []
        self.forward_port = forward_port
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        while not self.stop.is_set():
            try:
                value = self.socket.recv(65536)
                self.values.append(value)
                if self.forward_port is not None:
                    self.socket.sendto(value, ("127.0.0.1", self.forward_port))
            except socket.timeout:
                pass

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        self.socket.close()


def wait_event(process: Process, name: str):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if any(f'"event":"{name}"' in line for line in process.lines):
            return
        if process.child.poll() is not None:
            break
        time.sleep(0.01)
    raise AssertionError(f"missing {name}: {process.output()}")


def synthetic_ts() -> bytes:
    packets = []
    for index in range(7 * 80):
        packets.append(bytes((0x47, 0x1f, 0xff, 0x10 | (index & 15)))
                       + bytes((index & 255,)) * 184)
    return b"".join(packets)


def run(args):
    binary = str(args.binary.resolve())
    with tempfile.TemporaryDirectory(prefix="srt-multicast-media-") as directory:
        root = Path(directory)
        payload = synthetic_ts()
        chunks = [payload[i:i + 1316] for i in range(0, len(payload), 1316)]
        base = ports(6)
        environment = {"SRT_MULTICAST_SAMPLE_SECRET": "synthetic-multicast-media-key"}
        common = ["--endpoint", f"127.0.0.1:{base}", "--interface", "127.0.0.1",
                  "--latency-ms", "40"]
        if args.crypto != "plain":
            common += ["--passphrase-env", "SRT_MULTICAST_SAMPLE_SECRET",
                       "--crypto", args.crypto]
        run_seconds = 5 if args.ffmpeg else 3
        processes: list[Process] = []
        collectors: list[Collector] = []
        tap = None
        try:
            for port in (base + 3, base + 4):
                collectors.append(Collector(port))
            if args.ffmpeg:
                # Observe and forward the live encoder's exact input datagrams
                # so the two outputs can be compared with what entered SRT.
                tap = Collector(base + 5, base + 2)
            sender = Process([
                binary, "send", *common, "--multicast", f"239.255.133.11:{base + 1}",
                "--ttl", "0", "--min-receivers", "2", "--duration", str(run_seconds + 1),
                "--media-udp", f"127.0.0.1:{base + 2}",
            ], environment)
            processes.append(sender)
            sender.ready()
            receivers = []
            for index in range(2):
                receiver = Process([
                    binary, "receive", *common, "--duration", str(run_seconds),
                    "--media-udp", f"127.0.0.1:{base + 3 + index}",
                    "--record", str(root / f"received-{index}.ts"),
                ], environment)
                processes.append(receiver)
                receivers.append(receiver)
                wait_event(receiver, "media_ready")
            wait_event(sender, "media_ready")
            if args.ffmpeg:
                subprocess.run([
                    args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error",
                    "-re", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25",
                    "-re", "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000",
                    "-t", "2", "-map", "0:v:0", "-map", "1:a:0",
                    "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
                    "-g", "25", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "96k",
                    "-muxdelay", "0", "-muxpreload", "0", "-f", "mpegts",
                    f"udp://127.0.0.1:{base + 5}?pkt_size=1316",
                ], check=True, timeout=20)
            else:
                # Feed complete datagrams over time only after the sender
                # input and both receiver outputs are ready.
                started = time.monotonic()
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                    for index, chunk in enumerate(chunks):
                        time.sleep(max(0, started + 0.4 * index / len(chunks) - time.monotonic()))
                        source.sendto(chunk, ("127.0.0.1", base + 2))
            received_events = [receiver.finish() for receiver in receivers]
            sent_events = sender.finish()
            if tap:
                tap.close()
                chunks = tap.values
                payload = b"".join(chunks)
                assert payload and len(payload) % 188 == 0
            for collector in collectors:
                collector.close()
            for index, collector in enumerate(collectors):
                assert collector.values == chunks, "UDP datagram boundaries or payload changed"
                received = root / f"received-{index}.ts"
                assert received.read_bytes() == payload, "recording differs from input"
                summary = next(e for e in received_events[index] if e.get("event") == "media_summary")
                assert summary["bytes"] == len(payload)
                assert summary["datagrams"] == len(chunks)
                stats = [e for e in received_events[index] if e.get("event") == "multicast_stats"]
                assert stats[-1]["multicast_packets_received"] >= len(chunks)
                if args.ffmpeg:
                    subprocess.run([
                        args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror",
                        "-i", str(received), "-map", "0:v:0", "-map", "0:a:0", "-f", "null", "-",
                    ], check=True, timeout=15)
            multicast = [e for e in sent_events if e.get("event") == "multicast_stats"]
            assert any(e["members"] == 2 for e in multicast)
            assert multicast[-1]["multicast_packets_sent"] == len(chunks), "originals were duplicated"
            assert multicast[-1]["multicast_repeats"] == 0
            kind = "decoded video and audio" if args.ffmpeg else "datagram integrity"
            print(f"PASS {args.crypto}: {len(payload)} bytes, {len(chunks)} multicast originals, "
                  f"two exact receivers, {kind}")
        finally:
            for process in processes:
                process.close()
            for collector in collectors:
                if not collector.stop.is_set():
                    collector.close()
            if tap and not tap.stop.is_set():
                tap.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--crypto", choices=("plain", "ctr", "gcm"), default="plain")
    parser.add_argument("--ffmpeg", help="Also generate and decode MPEG-TS with video and audio")
    run(parser.parse_args())
