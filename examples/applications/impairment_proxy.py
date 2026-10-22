#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded single-peer IPv4 UDP relay for reproducible sample experiments.

Only original SRT source DATA in the selected direction is impaired. Handshakes, other traffic,
parity and retransmissions pass unchanged, making this a recovery experiment,
not a full network emulator. No SRT payload decryption is performed.
"""
from __future__ import annotations

import argparse
import heapq
import json
import selectors
import socket
import struct
import time
from pathlib import Path


def endpoint(value: str) -> tuple[str, int]:
    host, port = value.rsplit(":", 1)
    socket.inet_pton(socket.AF_INET, host)
    number = int(port)
    if not 1 <= number <= 65535:
        raise ValueError("port must be 1..65535")
    return host, number


def run(args: argparse.Namespace) -> None:
    counters = dict(original=0, dropped=0, delayed=0, forwarded=0, queue_dropped=0, peer_unreachable=0, outage_dropped=0)
    pending: list[tuple[float, int, bytes]] = []
    serial = 0
    peer = None
    start = time.monotonic()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as front, socket.socket(
        socket.AF_INET, socket.SOCK_DGRAM
    ) as back, selectors.DefaultSelector() as selector:
        front.bind(args.listen)
        back.connect(args.target)
        front.setblocking(False)
        back.setblocking(False)
        selector.register(front, selectors.EVENT_READ)
        selector.register(back, selectors.EVENT_READ)
        print(json.dumps({"event": "ready", "channel": "proxy"}), flush=True)
        while time.monotonic() - start < args.duration:
            now = time.monotonic()
            outage = args.outage_file is not None and args.outage_file.exists()
            while pending and pending[0][0] <= now:
                _, _, data = heapq.heappop(pending)
                if outage:
                    counters["outage_dropped"] += 1
                    continue
                try:
                    if args.impair_direction == "forward":
                        back.send(data)
                    else:
                        front.sendto(data, peer)
                    counters["forwarded"] += 1
                except BlockingIOError:
                    counters["queue_dropped"] += 1
                except (ConnectionRefusedError, ConnectionResetError):
                    counters["peer_unreachable"] += 1
            timeout = min(0.02, max(0, pending[0][0] - now)) if pending else 0.02
            for key, _ in selector.select(timeout):
                try:
                    data, source = key.fileobj.recvfrom(2048)
                except BlockingIOError:
                    continue
                except (ConnectionRefusedError, ConnectionResetError):
                    # Connected UDP can surface a late ICMP after a sample exits.
                    counters["peer_unreachable"] += 1
                    continue
                if len(data) < 16 or len(data) > 1500:
                    continue
                if outage:
                    counters["outage_dropped"] += 1
                    continue
                if key.fileobj is back:
                    if peer is None:
                        continue
                    if args.impair_direction == "forward":
                        try:
                            front.sendto(data, peer)
                        except BlockingIOError:
                            counters["queue_dropped"] += 1
                        except (ConnectionRefusedError, ConnectionResetError):
                            counters["peer_unreachable"] += 1
                        continue
                else:
                    if peer is None:
                        peer = source
                    if source != peer:
                        continue
                    if args.impair_direction == "reverse":
                        try:
                            back.send(data)
                        except BlockingIOError:
                            counters["queue_dropped"] += 1
                        except (ConnectionRefusedError, ConnectionResetError):
                            counters["peer_unreachable"] += 1
                        continue
                first, message = struct.unpack_from("!II", data)
                original = not (first & 0x80000000) and bool(message & 0x03FFFFFF) and not (message & 0x04000000)
                delay = 0.0
                if original:
                    counters["original"] += 1
                    index = counters["original"]
                    if args.drop_every and (index - 1) % args.drop_every < args.burst:
                        counters["dropped"] += 1
                        continue
                    delay = args.delay_ms / 1000
                    if args.reorder_every and index % args.reorder_every == 0:
                        delay += args.reorder_ms / 1000
                    if delay:
                        counters["delayed"] += 1
                if len(pending) >= 4096:
                    counters["queue_dropped"] += 1
                    continue
                serial += 1
                heapq.heappush(pending, (time.monotonic() + delay, serial, data))
        print(json.dumps({"event": "proxy_summary", "pending_at_stop": len(pending), **counters}), flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--listen", type=endpoint, default=("127.0.0.1", 9001))
    parser.add_argument("--target", type=endpoint, default=("127.0.0.1", 9000))
    parser.add_argument("--duration", type=float, default=30)
    parser.add_argument("--outage-file", type=Path, help="while this local marker exists, discard all traffic in both directions")
    parser.add_argument("--impair-direction", choices=["forward", "reverse"], default="forward")
    parser.add_argument("--drop-every", type=int, default=0)
    parser.add_argument("--burst", type=int, default=1)
    parser.add_argument("--delay-ms", type=float, default=0)
    parser.add_argument("--reorder-every", type=int, default=0)
    parser.add_argument("--reorder-ms", type=float, default=30)
    args = parser.parse_args()
    import math
    if not all(math.isfinite(v) for v in (args.duration, args.delay_ms, args.reorder_ms)):
        parser.error("timing values must be finite")
    if not 0 < args.duration <= 3600 or not 0 <= args.delay_ms <= 2000 or not 0 <= args.reorder_ms <= 2000:
        parser.error("duration must be 0..3600 seconds; delay/reorder 0..2000 ms")
    if args.drop_every < 0 or args.reorder_every < 0 or args.burst < 1 or (args.drop_every and args.burst > args.drop_every):
        parser.error("invalid loss or reorder period/burst")
    run(args)


if __name__ == "__main__":
    main()
