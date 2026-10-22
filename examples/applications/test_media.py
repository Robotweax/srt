#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real MPEG-TS through Backup/Broadcast: byte preservation and decoder acceptance."""
import argparse
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from process_support import Process, ports


def run(args, policy):
    with tempfile.TemporaryDirectory(prefix="srt-media-test-") as directory:
        root = Path(directory)
        source, received = root / "source.ts", root / "received.ts"
        subprocess.run([args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25", "-t", "2",
            "-c:v", "libx264", "-preset", "ultrafast", "-g", "25", "-pix_fmt", "yuv420p", "-f", "mpegts", str(source)], check=True, timeout=20)
        base = ports(3)
        executable = str(args.binary.resolve())
        processes = []
        try:
            listener = Process([executable, "receive", "--endpoint", f"127.0.0.1:{base}",
                "--media-udp", f"127.0.0.1:{base + 1}", "--duration", "5", "--record", str(received)])
            processes.append(listener)
            listener.ready()
            sender = Process([executable, "send", "--policy", policy,
                "--path", f"127.0.0.1:0,127.0.0.1:{base},100", "--path", f"127.0.0.1:0,127.0.0.1:{base},50",
                "--media-udp", f"127.0.0.1:{base + 2}", "--duration", "6", "--fail-after", "20", "--restore-after", "70"])
            processes.append(sender)
            deadline = time.monotonic() + 8
            while not any('"media_ready"' in line for line in sender.lines):
                if time.monotonic() >= deadline or sender.child.poll() is not None:
                    raise AssertionError(sender.output())
                time.sleep(0.01)
            payload = source.read_bytes()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                for offset in range(0, len(payload), 1316):
                    udp.sendto(payload[offset:offset + 1316], ("127.0.0.1", base + 2))
                    time.sleep(0.005)
            sent = sender.finish()
            got = listener.finish()
            assert payload == received.read_bytes(), "MPEG-TS bytes changed or were lost"
            assert any(e.get("event") == "path_rejoining" for e in sent)
            assert next(e for e in got if e.get("event") == "media_summary")["bytes"] == len(payload)
            subprocess.run([args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror",
                "-i", str(received), "-f", "null", "-"], check=True, timeout=10)
            print(f"{policy}: {len(payload)} MPEG-TS bytes preserved; decoded without errors")
        finally:
            for process in processes:
                process.close()


def reject_malformed(args):
    base = ports(3)
    binary = str(args.binary.resolve())
    processes = []
    try:
        receiver = Process([binary, "receive", "--endpoint", f"127.0.0.1:{base}",
            "--media-udp", f"127.0.0.1:{base + 1}", "--duration", "3"])
        processes.append(receiver)
        receiver.ready()
        sender = Process([binary, "send", "--policy", "backup",
            "--path", f"127.0.0.1:0,127.0.0.1:{base},100",
            "--path", f"127.0.0.1:0,127.0.0.1:{base},50",
            "--media-udp", f"127.0.0.1:{base + 2}", "--duration", "3"])
        processes.append(sender)
        deadline = time.monotonic() + 5
        while not any('"media_ready"' in line for line in sender.lines):
            if sender.child.poll() is not None or time.monotonic() > deadline:
                raise AssertionError(sender.output())
            time.sleep(0.01)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.sendto(bytes(188), ("127.0.0.1", base + 2))
        sender.child.wait(timeout=5)
        for reader in sender.readers:
            reader.join(timeout=2)
        assert sender.child.returncode != 0, "invalid MPEG-TS was accepted"
        assert "not synchronized MPEG-TS" in sender.output(), sender.output()
        print("malformed MPEG-TS rejected before forwarding")
    finally:
        for process in processes:
            process.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--ffmpeg", required=True)
    args = parser.parse_args()
    run(args, "backup")
    run(args, "broadcast")
    reject_malformed(args)
