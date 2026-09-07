#!/usr/bin/env python3
"""Black-box HSv5 interoperability smoke test against srt-live-transmit."""

from __future__ import annotations

import argparse
import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path


class UdpLossProxy:
    def __init__(self, listen_port: int, target_port: int) -> None:
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind(("127.0.0.1", listen_port))
        self._socket.settimeout(0.05)
        self._target = ("127.0.0.1", target_port)
        self._source: tuple[str, int] | None = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._data_packets = 0
        self.dropped = False

    def start(self) -> None:
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self._socket.close()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                payload, address = self._socket.recvfrom(65_535)
            except socket.timeout:
                continue
            if address == self._target:
                if self._source is not None:
                    self._socket.sendto(payload, self._source)
                continue

            self._source = address
            is_data = len(payload) >= 16 and (payload[0] & 0x80) == 0
            if is_data:
                self._data_packets += 1
                if self._data_packets == 2:
                    self.dropped = True
                    continue
            self._socket.sendto(payload, self._target)


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as candidate:
        candidate.bind(("127.0.0.1", 0))
        return int(candidate.getsockname()[1])


def resolve_program_path(path: Path) -> Path:
    """Return an absolute path so subprocess never falls back to PATH lookup."""
    resolved = path.expanduser().resolve(strict=True)
    if not resolved.is_file():
        raise FileNotFoundError(f"program is not a regular file: {resolved}")
    return resolved


def terminate(process: subprocess.Popen[str]) -> str:
    process.terminate()
    try:
        stdout, stderr = process.communicate(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate(timeout=2)
    return stdout + stderr


def robotweax_caller(reference: Path, robotweax: Path) -> None:
    port = free_udp_port()
    listener = subprocess.Popen(
        [
            str(reference),
            f"srt://127.0.0.1:{port}?mode=listener",
            "file://con",
            "-ll:error",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.2)
        result = subprocess.run(
            [str(robotweax), "caller", str(port)],
            capture_output=True,
            text=True,
            timeout=8,
            check=False,
        )
        if result.returncode != 0 or "CONNECTED" not in result.stdout:
            reference_log = terminate(listener)
            raise RuntimeError(
                "Robotweax caller -> Haivision listener failed\n"
                + result.stdout
                + result.stderr
                + reference_log
            )
    finally:
        if listener.poll() is None:
            terminate(listener)


def reference_caller(reference: Path, robotweax: Path) -> None:
    port = free_udp_port()
    listener = subprocess.Popen(
        [str(robotweax), "listener", str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    caller = None
    try:
        time.sleep(0.1)
        caller = subprocess.Popen(
            [
                str(reference),
                "file://con",
                f"srt://127.0.0.1:{port}?mode=caller",
                "-ll:error",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        stdout, stderr = listener.communicate(timeout=8)
        if listener.returncode != 0 or "CONNECTED" not in stdout:
            raise RuntimeError(
                "Haivision caller -> Robotweax listener failed\n" + stdout + stderr
            )
    finally:
        if listener.poll() is None:
            terminate(listener)
        if caller is not None and caller.poll() is None:
            terminate(caller)


def robotweax_sends_data(reference_peer: Path, robotweax: Path) -> None:
    port = free_udp_port()
    token = "robotweax-to-haivision-exact"
    listener = subprocess.Popen(
        [str(reference_peer), "listener-receive", str(port), token],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.1)
        sender = subprocess.run(
            [str(robotweax), "caller", str(port), f"send={token}"],
            capture_output=True,
            text=True,
            timeout=8,
            check=False,
        )
        stdout, stderr = listener.communicate(timeout=8)
        if sender.returncode != 0 or listener.returncode != 0:
            raise RuntimeError(
                "Robotweax -> Haivision data transfer failed\n"
                + sender.stdout + sender.stderr + stdout + stderr
            )
    finally:
        if listener.poll() is None:
            terminate(listener)


def reference_sends_data(reference_peer: Path, robotweax: Path) -> None:
    port = free_udp_port()
    token = "haivision-to-robotweax-exact"
    listener = subprocess.Popen(
        [str(robotweax), "listener", str(port), "receive"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.1)
        sender = subprocess.run(
            [str(reference_peer), "caller-send", str(port), token],
            capture_output=True,
            text=True,
            timeout=8,
            check=False,
        )
        stdout, stderr = listener.communicate(timeout=8)
        if sender.returncode != 0 or listener.returncode != 0 or token not in stdout:
            raise RuntimeError(
                "Haivision -> Robotweax data transfer failed\n"
                + sender.stdout + sender.stderr + stdout + stderr
            )
    finally:
        if listener.poll() is None:
            terminate(listener)


def reference_honors_negotiated_tsbpd(reference_peer: Path, robotweax: Path) -> None:
    port = free_udp_port()
    token = "haivision-tsbpd-gated"
    listener = subprocess.Popen(
        [str(robotweax), "listener", str(port), "receive-tsbpd=300"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.1)
        sender = subprocess.run(
            [str(reference_peer), "caller-send", str(port), token],
            capture_output=True,
            text=True,
            timeout=8,
            check=False,
        )
        stdout, stderr = listener.communicate(timeout=8)
        match = re.search(r"GATE_US=(\d+)", stdout)
        gate_us = int(match.group(1)) if match is not None else 0
        if (sender.returncode != 0 or listener.returncode != 0
                or token not in stdout or gate_us < 150_000):
            raise RuntimeError(
                "Haivision -> Robotweax negotiated TSBPD gate failed\n"
                + sender.stdout + sender.stderr + stdout + stderr
            )
    finally:
        if listener.poll() is None:
            terminate(listener)


def robotweax_recovers_loss(reference_peer: Path, robotweax: Path) -> None:
    port = free_udp_port()
    token = "loss-recovery-" + ("0123456789abcdef" * 190)
    listener = subprocess.Popen(
        [str(reference_peer), "listener-receive", str(port), token],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.1)
        sender = subprocess.run(
            [str(robotweax), "caller", str(port), f"send-loss={token}"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        stdout, stderr = listener.communicate(timeout=10)
        if sender.returncode != 0 or listener.returncode != 0:
            raise RuntimeError(
                "Robotweax NAK retransmission test failed\n"
                + sender.stdout + sender.stderr + stdout + stderr
            )
    finally:
        if listener.poll() is None:
            terminate(listener)


def haivision_honors_robotweax_drop_request(
    reference_peer: Path, robotweax: Path
) -> None:
    port = free_udp_port()
    token = "fresh-message-after-dropreq"
    listener = subprocess.Popen(
        [str(reference_peer), "listener-receive", str(port), token],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.1)
        sender = subprocess.run(
            [str(robotweax), "caller", str(port), f"send-drop={token}"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        stdout, stderr = listener.communicate(timeout=10)
        if sender.returncode != 0 or listener.returncode != 0:
            raise RuntimeError(
                "Haivision did not advance across Robotweax DROPREQ\n"
                + sender.stdout + sender.stderr + stdout + stderr
            )
    finally:
        if listener.poll() is None:
            terminate(listener)


def robotweax_livecc_sustained_rate(reference_peer: Path, robotweax: Path) -> None:
    port = free_udp_port()
    byte_count = 200_000
    input_rate = 200_000
    listener = subprocess.Popen(
        [str(reference_peer), "listener-measure-rate", str(port), str(byte_count)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.1)
        sender = subprocess.run(
            [
                str(robotweax), "caller", str(port),
                f"send-rate={byte_count},{input_rate}",
            ],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        stdout, stderr = listener.communicate(timeout=10)
        match = re.search(r"RATE_BPS=(\d+)", stdout)
        measured_rate = int(match.group(1)) if match is not None else 0
        if (sender.returncode != 0 or listener.returncode != 0
                or measured_rate < 180_000 or measured_rate > 300_000):
            raise RuntimeError(
                "Robotweax sustained-rate LiveCC test failed\n"
                + sender.stdout + sender.stderr + stdout + stderr
            )
    finally:
        if listener.poll() is None:
            terminate(listener)


def robotweax_livecc_dynamic_rate(reference_peer: Path, robotweax: Path) -> None:
    port = free_udp_port()
    byte_count = 200_000
    listener = subprocess.Popen(
        [str(reference_peer), "listener-measure-rate", str(port), str(byte_count)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.1)
        sender = subprocess.run(
            [
                str(robotweax), "caller", str(port),
                f"send-dynamic-rate={byte_count},100000,400000",
            ],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        stdout, stderr = listener.communicate(timeout=10)
        first_match = re.search(r"RATE1_BPS=(\d+)", stdout)
        second_match = re.search(r"RATE2_BPS=(\d+)", stdout)
        first_rate = int(first_match.group(1)) if first_match is not None else 0
        second_rate = int(second_match.group(1)) if second_match is not None else 0
        if (sender.returncode != 0 or listener.returncode != 0
                or first_rate < 80_000 or first_rate > 180_000
                or second_rate < 300_000 or second_rate > 650_000
                or second_rate < first_rate * 2):
            raise RuntimeError(
                "Robotweax dynamic LiveCC rate-change test failed\n"
                + sender.stdout + sender.stderr + stdout + stderr
            )
    finally:
        if listener.poll() is None:
            terminate(listener)


def injected_clock_skew_interop(reference_peer: Path, robotweax: Path) -> None:
    port = free_udp_port()
    message_count = 1_200
    listener = subprocess.Popen(
        [
            str(robotweax), "listener", str(port),
            f"receive-skew=20000,{message_count}",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.1)
        sender = subprocess.run(
            [str(reference_peer), "caller-clock-test", str(port), str(message_count)],
            capture_output=True,
            text=True,
            timeout=12,
            check=False,
        )
        stdout, stderr = listener.communicate(timeout=12)
        corrections_match = re.search(r"DRIFT_CORRECTIONS=(\d+)", stdout)
        total_match = re.search(r"DRIFT_TOTAL_US=(-?\d+)", stdout)
        corrections = int(corrections_match.group(1)) if corrections_match else 0
        total = int(total_match.group(1)) if total_match else 0
        if (sender.returncode != 0 or listener.returncode != 0
                or corrections == 0 or total == 0):
            raise RuntimeError(
                "Injected-clock-skew interoperability test failed\n"
                + sender.stdout + sender.stderr + stdout + stderr
            )
    finally:
        if listener.poll() is None:
            terminate(listener)


def robotweax_honors_haivision_drop_request(
    reference_peer: Path, robotweax: Path
) -> None:
    port = free_udp_port()
    token = "fresh-after-haivision-dropreq"
    listener = subprocess.Popen(
        [str(robotweax), "listener", str(port), f"receive-token={token}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.1)
        sender = subprocess.run(
            [str(reference_peer), "caller-drop-then-send", str(port), token],
            capture_output=True,
            text=True,
            timeout=12,
            check=False,
        )
        stdout, stderr = listener.communicate(timeout=12)
        if (sender.returncode != 0 or listener.returncode != 0
                or token not in stdout):
            raise RuntimeError(
                "Robotweax did not advance across Haivision DROPREQ\n"
                + sender.stdout + sender.stderr + stdout + stderr
            )
    finally:
        if listener.poll() is None:
            terminate(listener)


def reference_recovers_loss(reference_peer: Path, robotweax: Path) -> None:
    listener_port = free_udp_port()
    proxy_port = free_udp_port()
    token = "reverse-loss-" + ("fedcba9876543210" * 190)
    listener = subprocess.Popen(
        [str(robotweax), "listener", str(listener_port), f"receive-bytes={len(token)}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    proxy = UdpLossProxy(proxy_port, listener_port)
    proxy.start()
    try:
        time.sleep(0.1)
        sender = subprocess.run(
            [str(reference_peer), "caller-send-chunks", str(proxy_port), token],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        stdout, stderr = listener.communicate(timeout=10)
        if (sender.returncode != 0 or listener.returncode != 0
                or token not in stdout or not proxy.dropped):
            raise RuntimeError(
                "Haivision retransmission after Robotweax NAK failed\n"
                + sender.stdout + sender.stderr + stdout + stderr
            )
    finally:
        proxy.close()
        if listener.poll() is None:
            terminate(listener)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-probe", type=Path, required=True)
    parser.add_argument("--srt-live-transmit", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path)
    arguments = parser.parse_args()

    try:
        robotweax_probe = resolve_program_path(arguments.robotweax_probe)
        srt_live_transmit = resolve_program_path(arguments.srt_live_transmit)
        reference_peer = (
            resolve_program_path(arguments.reference_peer)
            if arguments.reference_peer is not None
            else None
        )

        robotweax_caller(srt_live_transmit, robotweax_probe)
        print("PASS Robotweax caller -> Haivision listener")
        reference_caller(srt_live_transmit, robotweax_probe)
        print("PASS Haivision caller -> Robotweax listener")
        if reference_peer is not None:
            robotweax_sends_data(reference_peer, robotweax_probe)
            print("PASS Robotweax -> Haivision exact message transfer")
            reference_sends_data(reference_peer, robotweax_probe)
            print("PASS Haivision -> Robotweax exact message transfer")
            reference_honors_negotiated_tsbpd(
                reference_peer, robotweax_probe
            )
            print("PASS Haivision -> Robotweax negotiated 300 ms TSBPD gate")
            robotweax_recovers_loss(reference_peer, robotweax_probe)
            print("PASS Robotweax retransmission after Haivision NAK")
            haivision_honors_robotweax_drop_request(
                reference_peer, robotweax_probe
            )
            print("PASS Haivision advances receive window after Robotweax DROPREQ")
            robotweax_livecc_sustained_rate(
                reference_peer, robotweax_probe
            )
            print("PASS Robotweax LiveCC sustained-rate pacing into Haivision")
            robotweax_livecc_dynamic_rate(
                reference_peer, robotweax_probe
            )
            print("PASS Robotweax dynamic LiveCC rate change into Haivision")
            injected_clock_skew_interop(
                reference_peer, robotweax_probe
            )
            print("PASS Robotweax drift correction under injected clock skew")
            robotweax_honors_haivision_drop_request(
                reference_peer, robotweax_probe
            )
            print("PASS Robotweax advances receive window after Haivision DROPREQ")
            reference_recovers_loss(reference_peer, robotweax_probe)
            print("PASS Haivision retransmission after Robotweax NAK")
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
