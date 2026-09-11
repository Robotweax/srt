#!/usr/bin/env python3
"""Public-API UDP/SRT/UDP datagram-integrity and rejection smoke tests."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import queue
import socket
import subprocess
import threading
import time


class Peer:
    def __init__(self, command: list[str], environment: dict[str, str]) -> None:
        self.program = command[0]
        self.process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, env=environment,
        )
        self.lines: list[str] = []
        self.events: queue.Queue[str] = queue.Queue()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.lines.append(line)
            self.events.put(line)
        self.events.put("EOF")

    def wait_for(self, prefix: str, contains: str = "") -> str:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                line = self.events.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if line.startswith(prefix) and contains in line:
                return line
            if line == "EOF":
                break
        raise AssertionError(
            f"missing {prefix}: program={self.program}, "
            f"pid={self.process.pid}, exit_code={self.process.poll()}\n"
            f"{''.join(self.lines)}"
        )

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
            raise AssertionError("bridge failed to stop")
        finally:
            self.reader.join(timeout=2)
            if self.process.stdout is not None:
                self.process.stdout.close()


def free_port(excluded: set[int]) -> int:
    for _ in range(30):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as candidate:
            candidate.bind(("127.0.0.1", 0))
            port = candidate.getsockname()[1]
            if port not in excluded:
                return int(port)
    raise RuntimeError("cannot allocate distinct loopback ports")


def run_bridge(demo: Path, reverse: bool = False, encrypted: bool = False,
               invalid: bytes | None = None, rendezvous: bool = False,
               multicast: bool = False, restart_sender: bool | None = None) -> None:
    environment = os.environ.copy()
    secret_name = "ROBOTWEAX_BRIDGE_TEST_SECRET"
    environment[secret_name] = "public-loopback-fixture-only"
    peers: list[Peer] = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink, \
            socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
        input_host = "239.255.42.1" if multicast else "127.0.0.1"
        output_host = "239.255.42.2" if multicast else "127.0.0.1"
        sink.bind(("0.0.0.0" if multicast else "127.0.0.1", 0))
        if multicast:
            sink.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                            socket.inet_aton(output_host) + socket.inet_aton("127.0.0.1"))
            source.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                              socket.inet_aton("127.0.0.1"))
            source.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        sink.settimeout(5)
        output_port = int(sink.getsockname()[1])
        input_port = free_port({output_port})
        srt_port = free_port({input_port, output_port})
        common = ["--srt-host", "127.0.0.1",
                  "--latency-ms", "40", "--stats-interval-ms", "200",
                  "--input-idle-ms", "400"]
        if restart_sender is not None or invalid is not None:
            common += ["--reconnect", "on", "--reconnect-delay-ms", "200",
                       "--connect-timeout-ms", "2000", "--peer-idle-timeout-ms", "1000"]
        if encrypted:
            common += ["--passphrase-env", secret_name]
        sender_command = [str(demo), "send", "--udp-port", str(input_port),
                          "--srt-mode", "listener" if reverse else "caller",
                          "--srt-port", str(srt_port), *common]
        receiver_command = [str(demo), "receive", "--udp-port", str(output_port),
                            "--srt-mode", "caller" if reverse else "listener",
                            "--srt-port", str(srt_port), *common]
        if rendezvous:
            second_port = free_port({input_port, output_port, srt_port})
            rv_common = [*common, "--srt-mode", "rendezvous",
                         "--srt-bind-host", "127.0.0.1"]
            sender_command = [str(demo), "send", "--udp-port", str(input_port),
                              "--srt-port", str(second_port), "--srt-local-port",
                              str(srt_port), *rv_common]
            receiver_command = [str(demo), "receive", "--udp-port", str(output_port),
                                "--srt-port", str(srt_port), "--srt-local-port",
                                str(second_port), *rv_common]
        sender_command += ["--udp-host", input_host]
        receiver_command += ["--udp-host", output_host]
        if multicast:
            sender_command += ["--udp-interface", "127.0.0.1"]
            receiver_command += ["--udp-interface", "127.0.0.1", "--udp-ttl", "3"]
        try:
            listener = Peer(sender_command if reverse else receiver_command, environment)
            peers.append(listener)
            listener.wait_for("CONNECTING SRT rendezvous" if rendezvous else "READY")
            caller = Peer(receiver_command if reverse else sender_command, environment)
            peers.append(caller)
            listener.wait_for("CONNECTED")
            caller.wait_for("CONNECTED")
            sender = listener if reverse else caller
            receiver = caller if reverse else listener
            if invalid is not None:
                source.sendto(invalid, (input_host, input_port))
                if sender.process.wait(timeout=5) != 1:
                    raise AssertionError("invalid UDP payload was not rejected")
                sender.reader.join(timeout=2)
                if "invalid MPEG-TS" not in "".join(sender.lines):
                    raise AssertionError(f"wrong rejection: {sender.lines}")
                sink.settimeout(0.15)
                try:
                    sink.recv(65536)
                except socket.timeout:
                    pass
                else:
                    raise AssertionError("invalid payload reached UDP output")
            else:
                # All supported datagram sizes, distinct payloads, exact ordering.
                # Receive each datagram before sending the next: no dependence on
                # host UDP receive-buffer capacity or a fixed startup sleep.
                for index in range(28):
                    ts_packet = b"\x47" + bytes([index]) * 187
                    payload = ts_packet * (1 + index % 7)
                    source.sendto(payload, (input_host, input_port))
                    actual = sink.recv(65536)
                    if actual != payload:
                        raise AssertionError(f"payload/datagram mismatch at {index}")
                # A short paced stream also exercises multiple in-flight messages,
                # unlike the request/receive checks above. This is not a benchmark.
                stream = [(b"\x47" + bytes([index]) * 187) * 7 for index in range(128)]
                errors: queue.Queue[Exception] = queue.Queue()

                def feed() -> None:
                    try:
                        for payload in stream:
                            source.sendto(payload, (input_host, input_port))
                            time.sleep(0.002)
                    except Exception as error:
                        errors.put(error)

                feeder = threading.Thread(target=feed, daemon=True)
                feeder.start()
                try:
                    for index, payload in enumerate(stream):
                        if sink.recv(65536) != payload:
                            raise AssertionError(f"paced stream mismatch at {index}")
                finally:
                    feeder.join(timeout=5)
                if feeder.is_alive() or not errors.empty():
                    raise AssertionError("UDP source thread did not complete successfully")
                sink.settimeout(0.1)
                try:
                    sink.recv(65536)
                except socket.timeout:
                    pass
                else:
                    raise AssertionError("duplicate output datagram")
                if any(peer.process.poll() is not None for peer in peers):
                    raise AssertionError("bridge stopped during live transfer")
                for peer in (sender, receiver):
                    active = peer.wait_for("STATUS", "input=ACTIVE")
                    fields = dict(part.split("=", 1) for part in active.split()[1:])
                    assert fields["stats_available"] == "true", active
                    assert float(fields["output_payload_mbps"]) > 0, active
                    assert int(fields["forwarded_bytes"]) > 0, active
                    assert float(fields["rtt_ms"]) >= 0, active
                    idle = peer.wait_for("STATUS", "input=IDLE")
                    assert "connection=CONNECTED" in idle, idle
                    assert "stats_available=true" in idle, idle
                if restart_sender is not None:
                    retired = sender if restart_sender else receiver
                    survivor = receiver if restart_sender else sender
                    retired.close()
                    peers.remove(retired)
                    survivor.wait_for("RECONNECTING")
                    replacement = Peer(sender_command if restart_sender else receiver_command, environment)
                    peers.append(replacement)
                    recovered = survivor.wait_for("CONNECTED", "generation=2")
                    assert "recovery_ms=" in recovered, recovered
                    replacement.wait_for("CONNECTED")
                    payload = (b"\x47" + b"\xab" * 187) * 7
                    source.sendto(payload, (input_host, input_port))
                    sink.settimeout(5)
                    assert sink.recv(65536) == payload, "restart payload mismatch"
        except Exception as error:
            raise AssertionError(
                f"reverse={reverse}, rendezvous={rendezvous}, encrypted={encrypted}, "
                f"invalid={invalid is not None}: "
                f"{error}\n" + "\n".join("".join(peer.lines) for peer in peers)
            ) from error
        finally:
            for peer in reversed(peers):
                peer.close()


def run_failure_policy(demo: Path) -> None:
    """No-server retry, default-off disconnect and fatal auth rejection."""
    for scenario in ("late-listener", "reconnect-off", "wrong-passphrase"):
        input_port = free_port(set())
        output_port = free_port({input_port})
        srt_port = free_port({input_port, output_port})
        common = ["--srt-host", "127.0.0.1", "--srt-port", str(srt_port),
                  "--connect-timeout-ms", "1000", "--peer-idle-timeout-ms", "1000"]
        sender_command = [str(demo), "send", "--udp-port", str(input_port), *common]
        receiver_command = [str(demo), "receive", "--udp-port", str(output_port), *common]
        sender_env = os.environ.copy()
        receiver_env = os.environ.copy()
        if scenario != "reconnect-off":
            sender_command += ["--reconnect", "on", "--reconnect-delay-ms", "200"]
        if scenario == "wrong-passphrase":
            sender_command += ["--passphrase-env", "BRIDGE_POLICY_TEST_SECRET"]
            receiver_command += ["--passphrase-env", "BRIDGE_POLICY_TEST_SECRET"]
            sender_env["BRIDGE_POLICY_TEST_SECRET"] = "public-wrong-fixture"
            receiver_env["BRIDGE_POLICY_TEST_SECRET"] = "public-right-fixture"
        peers: list[Peer] = []
        try:
            if scenario == "late-listener":
                sender = Peer(sender_command, sender_env)
                peers.append(sender)
                sender.wait_for("RECONNECTING")
            receiver = Peer(receiver_command, receiver_env)
            peers.append(receiver)
            receiver.wait_for("READY")
            if scenario != "late-listener":
                sender = Peer(sender_command, sender_env)
                peers.append(sender)
            if scenario != "wrong-passphrase":
                sender.wait_for("CONNECTED")
                receiver.wait_for("CONNECTED")
            if scenario == "reconnect-off":
                receiver.close()
                peers.remove(receiver)
            if scenario != "late-listener":
                assert sender.process.wait(timeout=5) == 1, scenario
                sender.reader.join(timeout=2)
                assert not any(line.startswith("RECONNECTING") for line in sender.lines), sender.lines
            if scenario == "wrong-passphrase":
                assert not any("public-wrong-fixture" in line or "public-right-fixture" in line
                               for peer in peers for line in peer.lines), "secret in logs"
        except Exception as error:
            raise AssertionError(f"{scenario}: {error}\n" + "\n".join("".join(p.lines) for p in peers)) from error
        finally:
            for peer in reversed(peers):
                peer.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", type=Path, required=True)
    demo = parser.parse_args().demo.resolve()
    help_result = subprocess.run([str(demo), "--help"], capture_output=True, timeout=5)
    assert help_result.returncode == 0
    invalid_cli = subprocess.run(
        [str(demo), "send", "--udp-port", "0", "--srt-port", "9000"],
        capture_output=True, timeout=5,
    )
    assert invalid_cli.returncode == 1
    for extra in (["--passphrase-env", ""],
                  ["--passphrase-env", "ROBOTWEAX_UNSET_TEST_SECRET"],
                  ["--srt-mode", "rendezvous"],
                  ["--srt-mode", "unknown"],
                  ["--srt-local-port", "9001"],
                  ["--srt-bind-host", "127.0.0.1"],
                  ["--srt-mode", "rendezvous", "--srt-local-port", "9001",
                   "--stream-id", "not-supported-in-rendezvous"],
                  ["--udp-interface", "127.0.0.1"],
                  ["--udp-host", "239.1.2.3", "--udp-ttl", "2"],
                  ["--reconnect", "maybe"],
                  ["--stats-interval-ms", "0"]):
        environment = os.environ.copy()
        environment.pop("ROBOTWEAX_UNSET_TEST_SECRET", None)
        rejected = subprocess.run(
            [str(demo), "send", "--udp-port", str(free_port(set())),
             "--srt-port", "9000", *extra],
            capture_output=True, timeout=5, env=environment,
        )
        assert rejected.returncode == 1, extra
    for reverse, encrypted in ((False, False), (True, False), (False, True)):
        run_bridge(demo, reverse=reverse, encrypted=encrypted)
    for reverse in (False, True):
        for encrypted in (False, True):
            run_bridge(demo, reverse=reverse, encrypted=encrypted, rendezvous=True)
    for invalid in (b"", b"not MPEG-TS", b"\x00" * 188,
                    (b"\x47" + b"\x00" * 187) * 8):
        run_bridge(demo, invalid=invalid)
    run_bridge(demo, multicast=True)
    for reverse in (False, True):
        for restart_sender in (False, True):
            run_bridge(demo, reverse=reverse, restart_sender=restart_sender)
    for restart_sender in (False, True):
        run_bridge(demo, rendezvous=True, restart_sender=restart_sender)
    run_failure_policy(demo)
    print("PASS UDP->SRT->UDP: exact datagrams, all SRT roles, AES-CTR, multicast, stats, reconnect, invalid payload rejection")


if __name__ == "__main__":
    main()
