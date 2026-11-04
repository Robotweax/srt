# SPDX-License-Identifier: MIT
"""Bounded process and loopback-port helpers for sample launchers and tests."""
from __future__ import annotations
import json
import os
import queue
import secrets
import socket
import subprocess
import threading
import time
from collections import deque
from pathlib import Path

class Process:
    def __init__(self, command: list[str], environment: dict[str, str] | None = None,
                 log_prefix: Path | None = None):
        env = {**os.environ, **(environment or {})}
        self.logs = []
        try:
            if log_prefix is not None:
                log_prefix.parent.mkdir(parents=True, exist_ok=True)
                for suffix in ("stdout", "stderr"):
                    self.logs.append(Path(str(log_prefix) + f".{suffix}.log").open("w", encoding="utf-8"))
            self.child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        except BaseException:
            for log in self.logs:
                log.close()
            raise
        self.lines: list[str] = []
        self.errors: list[str] = []
        self.events: queue.Queue[dict] = queue.Queue()
        self.observations = deque(maxlen=4096)
        self.readers = [threading.Thread(target=self.read, args=(self.child.stdout, self.lines, True,
                         self.logs[0] if self.logs else None), daemon=True),
                        threading.Thread(target=self.read, args=(self.child.stderr, self.errors, False,
                         self.logs[1] if self.logs else None), daemon=True)]
        for reader in self.readers:
            reader.start()

    def read(self, stream, destination, events, log=None):
        for line in stream:
            destination.append(line)
            if log is not None:
                log.write(line)
                log.flush()
            if events:
                self.observations.append((time.monotonic(), line))
                try:
                    self.events.put(json.loads(line))
                except json.JSONDecodeError:
                    pass

    def ready(self, count=1):
        deadline = time.monotonic() + 10
        while count and time.monotonic() < deadline:
            try:
                item = self.events.get(timeout=0.1)
                if item.get("event") == "ready":
                    count -= 1
            except queue.Empty:
                if self.child.poll() is not None:
                    break
        if count:
            raise RuntimeError("readiness failed: " + self.output())

    def finish(self, timeout=30):
        try:
            self.child.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError("sample completion timed out: " + self.output()) from error
        for reader in self.readers:
            reader.join(timeout=2)
        if self.child.returncode:
            raise RuntimeError("sample failed: " + self.output())
        return [json.loads(line) for line in self.lines]

    def output(self):
        return "".join(self.lines + self.errors)

    def close(self):
        if self.child.poll() is None:
            self.child.kill()
        self.child.wait(timeout=5)
        for reader in self.readers:
            reader.join(timeout=2)
        self.child.stdout.close()
        self.child.stderr.close()
        for log in self.logs:
            log.close()


def ports(count=1):
    if not 1 <= count <= 64:
        raise ValueError("sample port count must be 1..64")
    for _ in range(100):
        sockets = []
        try:
            # Keep planned listeners below the usual dynamic-port ranges.
            # In particular, Windows can assign the next ephemeral port to
            # a peer or relay back socket before its sibling listener binds.
            base = 10000 + secrets.randbelow(20000)
            for offset in range(count):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sockets.append(sock)
                sock.bind(("127.0.0.1", base + offset))
            return base
        except OSError:
            continue
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError("cannot allocate sample ports")


