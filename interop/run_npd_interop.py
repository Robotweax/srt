#!/usr/bin/env python3
"""Black-box NPD qualification through public-API peers and a UDP relay.

Reference peers are built from api_peer.cpp against a pinned public srt.h/library.
No reference implementation source is inspected by this runner. Byte comparisons
cover DATA payloads and handshake extension records; ephemeral socket IDs,
sequence numbers, cookies and timestamps are deliberately not compared.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import threading
import time


def corpus() -> bytes:
    null = bytes.fromhex("471fff10") + b"\xff" * 184
    real = bytes.fromhex("47010010") + bytes(range(184))
    noncanonical = null[:3] + b"\x11" + null[4:]
    return b"".join((null * 7, null * 3 + real + null * 3,
                     real * 7, noncanonical * 7) * 16)


def expected_source_bytes(data: bytes) -> int:
    """Independent wire-length oracle for 1316-byte public-API messages."""
    if not data:
        raise ValueError("qualification input must not be empty")
    null = bytes.fromhex("471fff10") + b"\xff" * 184
    total = 0
    for offset in range(0, len(data), 1316):
        message = data[offset:offset + 1316]
        deleted = 0
        if len(message) % 188 == 0 and all(message[i] == 0x47 for i in range(0, len(message), 188)):
            deleted = sum(message[i:i + 188] == null for i in range(0, len(message), 188))
        total += len(message) + 1 - deleted * 188
    return total


def events(text: str) -> list[dict]:
    result = []
    for line in text.splitlines():
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict):
            result.append(value)
    return result


class Relay:
    def __init__(self, target_port: int):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(0.05)
        self.port = self.socket.getsockname()[1]
        self.target = ("127.0.0.1", target_port)
        self.caller = None
        self.stop = threading.Event()
        self.packets: list[tuple[str, bytes]] = []
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        while not self.stop.is_set():
            try:
                data, address = self.socket.recvfrom(65535)
            except socket.timeout:
                continue
            if address == self.target:
                direction, destination = "listener", self.caller
            else:
                self.caller = address
                direction, destination = "caller", self.target
            self.packets.append((direction, data))
            if destination:
                self.socket.sendto(data, destination)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        self.socket.close()

    def summary(self):
        payloads = {"caller": [], "listener": []}
        records = {"caller": set(), "listener": set()}
        parity_bytes = 0
        rejections = {"caller": [], "listener": []}
        seen = set()
        for direction, packet in self.packets:
            if len(packet) < 16:
                continue
            if packet[0] & 0x80 == 0:
                seq, message = struct.unpack_from("!II", packet)
                if message & 0x03ffffff == 0:
                    parity_bytes += len(packet) - 16
                elif (direction, seq) not in seen:
                    seen.add((direction, seq))
                    payloads[direction].append(packet[16:])
            elif packet[:2] == b"\x80\x00" and len(packet) >= 64:
                request = struct.unpack_from("!I", packet, 36)[0]
                if 1000 <= request < 2000:
                    rejections[direction].append(request)
                rest = packet[64:]
                while len(rest) >= 4:
                    command, words = struct.unpack_from("!HH", rest)
                    size = 4 + 4 * words
                    if size > len(rest):
                        break
                    records[direction].add((command, rest[4:size].hex()))
                    rest = rest[size:]
        return {
            "rejections": rejections,
            "source_payload_bytes": sum(len(p) for values in payloads.values() for p in values),
            "parity_payload_bytes": parity_bytes,
            "payload_hashes": {d: [hashlib.sha256(p).hexdigest() for p in ps] for d, ps in payloads.items()},
            "extension_records": {d: sorted(rs) for d, rs in records.items()},
        }


def run_case(directory, name, caller_peer, listener_peer, caller_policy,
             listener_policy, *, reverse=False, reject=False, fec=False, data=None):
    directory = directory / name
    directory.mkdir(parents=True, exist_ok=True)
    source, output = directory / "input.ts", directory / "output.ts"
    source.write_bytes(corpus() if data is None else data)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserved:
        reserved.bind(("127.0.0.1", 0))
        port = reserved.getsockname()[1]
    relay = Relay(port)

    def command(peer, role, policy):
        sender = (role == "listener") == reverse
        args = [str(peer), role, "--host", "127.0.0.1", "--port", str(port if role == "listener" else relay.port),
                "--bytes", str(source.stat().st_size), "--chunk-size", "1316", "--receive-size", "1500",
                "--timeout-ms", "1500", "--latency-ms", "40", "--shutdown-grace-ms", "200", "--data-role", "sender" if sender else "receiver",
                "--input" if sender else "--output", str(source if sender else output)]
        if policy is not None:
            args += ["--npd-policy", str(policy)]
        if fec:
            args += ["--packet-filter", "fec,cols:4,rows:1,arq:always"]
        return args

    listener_command = command(listener_peer, "listener", listener_policy)
    caller_command = command(caller_peer, "caller", caller_policy)
    with (directory / "listener.stdout").open("w") as out, (directory / "listener.stderr").open("w") as err:
        listener = subprocess.Popen(listener_command, stdout=out, stderr=err, text=True)
        try:
            deadline = time.monotonic() + 5
            while not any(e.get("event") == "ready" for e in events((directory / "listener.stdout").read_text())):
                if listener.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(f"{name}: listener not ready")
                time.sleep(0.01)
            relay.thread.start()
            caller = subprocess.run(caller_command, capture_output=True, text=True, timeout=8)
            (directory / "caller.stdout").write_text(caller.stdout)
            (directory / "caller.stderr").write_text(caller.stderr)
            if reject:
                if caller.returncode == 0:
                    raise RuntimeError(f"{name}: unexpected connection success")
            else:
                listener.wait(timeout=8)
                if caller.returncode or listener.returncode or output.read_bytes() != source.read_bytes():
                    raise RuntimeError(f"{name}: transfer failed or payload differs")
        finally:
            if listener.poll() is None:
                listener.terminate()
            listener.wait(timeout=5)
            if relay.thread.ident is not None:
                relay.close()
            else:
                relay.socket.close()
    summary = relay.summary()
    if reject:
        if caller_policy == 1:
            if not any(command == 0x7f15 for command, _ in summary["extension_records"]["caller"]):
                raise RuntimeError(f"{name}: required caller never offered NPD")
            if any(command == 0x7f15 for command, _ in summary["extension_records"]["listener"]):
                raise RuntimeError(f"{name}: reference unexpectedly echoed NPD")
            if "reject_reason=4" not in caller.stderr:
                raise RuntimeError(f"{name}: failure was not the expected missing-echo rejection")
        elif 1004 not in summary["rejections"]["listener"]:
            raise RuntimeError(f"{name}: listener did not reject the absent offer as ROGUE")
    summary.update(name=name, rejected=reject, reverse=reverse,
                   caller_command=caller_command, listener_command=listener_command,
                   caller_exit=caller.returncode, listener_exit=listener.returncode)
    (directory / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--input", type=Path, help="optional stream corpus; read in 1316-byte messages")
    args = parser.parse_args()
    candidate = args.robotweax_peer.resolve(strict=True)
    data = corpus() if args.input is None else args.input.read_bytes()
    expected = expected_source_bytes(data)
    results = []
    for reverse in (False, True):
        for fec in (False, True):
            stem = f"self-{'reverse' if reverse else 'forward'}-{'fec' if fec else 'arq'}"
            off = run_case(args.output, stem + "-off", candidate, candidate, 0, 0, reverse=reverse, fec=fec, data=data)
            active = run_case(args.output, stem + "-npd", candidate, candidate, 1, 2, reverse=reverse, fec=fec, data=data)
            if off["source_payload_bytes"] != len(data) or active["source_payload_bytes"] != expected:
                raise RuntimeError("source bytes differ from independent NPD length oracle")
            if args.input is None and fec and active["parity_payload_bytes"] <= off["parity_payload_bytes"]:
                raise RuntimeError("NPD parity must include the additional prefix byte")
            results += [off, active]
    for index, reference in enumerate(args.reference_peer):
        reference = reference.resolve(strict=True)
        for reverse in (False, True):
            stem = f"reference-{index}-{'reverse' if reverse else 'forward'}"
            off = run_case(args.output, stem + "-off", reference, candidate, None, 0, reverse=reverse, data=data)
            optional = run_case(args.output, stem + "-optional", reference, candidate, None, 2, reverse=reverse, data=data)
            for field in ("payload_hashes", "extension_records"):
                if off[field] != optional[field]:
                    raise RuntimeError(f"{stem}: IF_OFFERED changed {field} for stock caller")
            results += [off, optional]
        results.append(run_case(args.output, f"reference-{index}-required-caller", candidate, reference, 1, None, reject=True, data=data))
        results.append(run_case(args.output, f"reference-{index}-required-listener", reference, candidate, None, 1, reject=True, data=data))
    report = {"input": {"path": str(args.input.resolve()) if args.input else None,
                        "sha256": hashlib.sha256(data).hexdigest(), "application_bytes": len(data),
                        "expected_npd_source_bytes": expected},
              "binaries": {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in [candidate, *args.reference_peer]}, "cases": results}
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"validated {len(results)} black-box NPD cases")


if __name__ == "__main__":
    main()
