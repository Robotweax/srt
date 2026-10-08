# SPDX-License-Identifier: MIT
"""Bounded loopback relay for the group diagnostic's all-path tail regression."""
import select
import socket
import threading


class TailLossRelay:
    def __init__(self, port, tails):
        self.target = ("127.0.0.1", port)
        self.tails = frozenset(tails)
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.front.bind(("127.0.0.1", 0))
        self.port = self.front.getsockname()[1]
        self.clients = {}
        self.upstreams = {}
        self.dropped = set()
        self.error = None
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while not self.stopped.is_set():
                ready, _, _ = select.select([self.front, *self.upstreams], [], [], 0.02)
                for source in ready:
                    packet, address = source.recvfrom(65535)
                    if source is self.front:
                        if address not in self.clients:
                            if len(self.clients) == 64:
                                raise RuntimeError("relay member bound")
                            upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                            upstream.bind(("127.0.0.1", 0))
                            self.clients[address] = upstream
                            self.upstreams[upstream] = address
                        # DATA header is 16 bytes; the diagnostic's first
                        # payload word is a little-endian application index.
                        if len(packet) >= 24 and not packet[0] & 0x80:
                            index = int.from_bytes(packet[16:24], "little")
                            key = (address, index)
                            if index in self.tails and key not in self.dropped:
                                self.dropped.add(key)
                                continue
                        self.clients[address].sendto(packet, self.target)
                    elif address == self.target:
                        self.front.sendto(packet, self.upstreams[source])
        except Exception as error:
            self.error = error

    def validate(self, members):
        if self.error is not None:
            raise RuntimeError("tail relay failed") from self.error
        expected = {(address, index) for address in self.clients for index in self.tails}
        if len(self.clients) != members or self.dropped != expected:
            raise ValueError("tail loss was not injected on every path and phase")
        return len(self.dropped)

    def close(self):
        self.stopped.set()
        self.thread.join(timeout=2)
        if self.thread.is_alive():
            raise RuntimeError("tail relay did not stop")
        for upstream in self.upstreams:
            upstream.close()
        self.front.close()
