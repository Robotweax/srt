#!/usr/bin/env python3
"""Verify peer Stream IDs cannot inject terminal control text in either demo."""
from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path

from test_message_demo import command_output, reserve_udp_port, wait_for_ready


def check_stream_id(demo: Path, file_mode: bool, directory: Path) -> None:
    # ASCII control bytes survive both Windows narrow argv and UTF-8 argv.
    # Non-ASCII escaping is covered byte-for-byte in test_demo_text.cpp.
    port = reserve_udp_port()
    source = directory / "input.bin"
    source.write_bytes(b"fixture")
    listen_extra = (["--output", str(directory / "output.bin"), "--max-bytes", "64"]
                    if file_mode else [])
    call_extra = ["--input", str(source)] if file_mode else ["--message", "fixture"]
    listener = subprocess.Popen(
        [str(demo), "listener", "--bind", "127.0.0.1", "--port", str(port),
         "--expect-stream-id", "expected", "--timeout-ms", "1000", *listen_extra],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        wait_for_ready(listener)
        subprocess.run(
            [str(demo), "caller", "--host", "127.0.0.1", "--port", str(port),
             "--stream-id", "peer\nFORGED\x1b]52;c;demo\x07\x7f", "--timeout-ms", "1000",
             *call_extra], capture_output=True, text=True, timeout=10, check=False)
        _, stderr = command_output(listener)
        expected = r"error: unexpected Stream ID: peer\x0aFORGED\x1b]52;c;demo\x07\x7f" + "\n"
        if (listener.returncode != 1 or not stderr.endswith(expected)
                or "\x1b" in stderr or "\x07" in stderr
                or "\nFORGED" in stderr):
            raise RuntimeError(f"unexpected peer diagnostic: {stderr!r}")
    finally:
        if listener.poll() is None:
            listener.kill()
            listener.communicate()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--message-demo", required=True, type=Path)
    parser.add_argument("--file-demo", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        check_stream_id(args.message_demo, False, directory)
        check_stream_id(args.file_demo, True, directory)


if __name__ == "__main__":
    main()
