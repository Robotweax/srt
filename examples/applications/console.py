#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Loopback-only controls and real media playback for the SRT sample applications."""
from __future__ import annotations

import argparse
import hmac
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import mimetypes
import os
from pathlib import Path
import re
import secrets
import shutil
from socketserver import TCPServer
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit

from process_support import Process, ports

ROOT = Path(__file__).resolve().parent
FILTERS = {"arq": "", "row": "fec,cols:4,rows:1,arq:never",
           "column": "fec,cols:4,rows:-4,layout:even,arq:never",
           "matrix": "fec,cols:4,rows:4,layout:even,arq:onreq"}


def configuration(data):
    if not isinstance(data, dict) or set(data) - {"kind", "duration", "latency", "policy", "filter", "drop_every", "burst", "source"}:
        raise ValueError("unknown configuration field")
    result = {"kind": "media", "duration": 18, "latency": 120, "policy": "backup",
              "filter": "arq", "drop_every": 0, "burst": 1, "source": "testcard", **data}
    for key, choices in {"kind": ("media", "recovery", "device"), "policy": ("backup", "broadcast"),
                         "filter": tuple(FILTERS), "source": ("testcard", "file")}.items():
        if result[key] not in choices:
            raise ValueError(f"invalid {key}")
    for key, low, high in [("duration", 10, 120), ("latency", 60, 1000), ("drop_every", 0, 10000), ("burst", 1, 100)]:
        if type(result[key]) is not int or not low <= result[key] <= high:
            raise ValueError(f"{key} must be {low}..{high}")
    if result["drop_every"] and result["burst"] > result["drop_every"]:
        raise ValueError("burst must not exceed drop period")
    return result


class Job:
    def __init__(self, args, config):
        self.args, self.config = args, config
        self.directory = Path(tempfile.mkdtemp(prefix="srt-console-", dir=args.output_dir))
        self.lock = threading.RLock()
        self.stop_requested = threading.Event()
        self.children = []
        self.state, self.error = "starting", ""
        self.jpeg = b""
        self.decoded_frames = 0
        self.decoder = None
        self.decoder_thread = None
        self.markers = [self.directory / f"path-{i}.paused" for i in range(2)]
        self.thread = threading.Thread(target=self.run, daemon=True)

    def binary(self, name):
        return str(self.args.binary_dir / (f"robotweax_srt_{name}" + (".exe" if os.name == "nt" else "")))

    def spawn(self, name, command):
        with self.lock:
            if self.stop_requested.is_set():
                raise RuntimeError("stopped")
            process = Process(command)
            self.children.append((name, process))
            return process

    def wait(self, process, timeout):
        deadline = time.monotonic() + timeout
        while process.child.poll() is None:
            if self.stop_requested.wait(0.05):
                raise RuntimeError("stopped")
            if time.monotonic() > deadline:
                raise RuntimeError("sample process deadline exceeded")
        if process.child.returncode:
            raise RuntimeError(process.output()[-3000:] or f"sample exit {process.child.returncode}")

    def start_decoder(self, port):
        log = open(self.directory / "decoder.log", "wb")
        with self.lock:
            if self.stop_requested.is_set():
                log.close()
                raise RuntimeError("stopped")
            self.decoder = subprocess.Popen([self.args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "warning",
                "-probesize", "32768", "-analyzeduration", "500000", "-i",
                f"udp://127.0.0.1:{port}?fifo_size=4096&overrun_nonfatal=1",
                "-an", "-vf", "fps=8,scale=640:-2", "-c:v", "mjpeg", "-q:v", "5", "-f", "image2pipe", "pipe:1"],
                stdout=subprocess.PIPE, stderr=log)
        log.close()
        def read_frames():
            buffer = bytearray()
            while True:
                chunk = self.decoder.stdout.read(4096)
                if not chunk:
                    break
                buffer.extend(chunk)
                while True:
                    begin, end = buffer.find(b"\xff\xd8"), buffer.find(b"\xff\xd9")
                    if begin < 0 or end < begin:
                        break
                    frame = bytes(buffer[begin:end + 2])
                    del buffer[:end + 2]
                    with self.lock:
                        self.jpeg = frame
                        self.decoded_frames += 1
                if len(buffer) > 2 * 1024 * 1024:
                    buffer.clear()
        self.decoder_thread = threading.Thread(target=read_frames, daemon=True)
        self.decoder_thread.start()

    def proxy(self, name, listen, target, direction="forward", marker=None, burst=None):
        command = [sys.executable, str(ROOT / "impairment_proxy.py"), "--listen", f"127.0.0.1:{listen}",
                   "--target", f"127.0.0.1:{target}", "--duration", str(self.config["duration"] + 15),
                   "--drop-every", str(self.config["drop_every"]), "--burst", str(burst or self.config["burst"]),
                   "--impair-direction", direction]
        if marker:
            command += ["--outage-file", str(marker)]
        process = self.spawn(name, command)
        process.ready()
        return process

    def media(self):
        if not self.args.ffmpeg:
            raise RuntimeError("FFmpeg is required for media; restart with --ffmpeg PATH")
        duration = self.config["duration"]
        source = self.directory / "source.ts"
        command = [self.args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y"]
        if self.config["source"] == "file":
            if not self.args.media:
                raise ValueError("no input file configured; restart with --media PATH")
            command += ["-stream_loop", "-1", "-i", str(self.args.media)]
        else:
            command += ["-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"]
        command += ["-t", str(duration - 6), "-map", "0:v:0", "-map", "0:a?" if self.config["source"] == "file" else "1:a:0",
                    "-vf", "scale=640:-2", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                    "-g", "25", "-c:a", "aac", "-f", "mpegts", str(source)]
        self.wait(self.spawn("prepare-media", command), 60)
        base = ports(5)
        self.start_decoder(base + 4)
        common = ["--latency-ms", str(self.config["latency"])]
        receiver = self.spawn("receiver", [self.binary("contribution"), "receive", "--endpoint", f"127.0.0.1:{base}",
            "--media-udp", f"127.0.0.1:{base + 4}", "--duration", str(duration), "--record", str(self.directory / "received.ts"), *common])
        receiver.ready()
        for i in range(2):
            self.proxy(f"relay-{i + 1}", base + 1 + i, base, marker=self.markers[i])
        sender = self.spawn("sender", [self.binary("contribution"), "send", "--policy", self.config["policy"],
            "--path", f"127.0.0.1:0,127.0.0.1:{base + 1},100", "--path", f"127.0.0.1:0,127.0.0.1:{base + 2},50",
            "--media-udp", f"127.0.0.1:{base + 3}", "--duration", str(duration + 2), *common])
        # Do not start feeding UDP before the SRT group and input bind exist.
        deadline = time.monotonic() + 12
        while not any('"media_ready"' in line for line in sender.lines):
            if sender.child.poll() is not None or self.stop_requested.wait(0.05) or time.monotonic() > deadline:
                raise RuntimeError("media sender was not ready: " + sender.output()[-1000:])
        self.state = "running"
        encoder = self.spawn("source", [self.args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-re", "-i", str(source),
            "-map", "0", "-c", "copy", "-f", "mpegts", f"udp://127.0.0.1:{base + 3}?pkt_size=1316"])
        self.wait(encoder, duration + 10)
        self.wait(receiver, duration + 10)
        self.wait(sender, duration + 10)
        if self.decoded_frames == 0:
            raise RuntimeError("receiver produced no live decoded preview frames; inspect decoder.log")
        self.wait(self.spawn("recording", [self.args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
            "-i", str(self.directory / "received.ts"), "-c", "copy", "-movflags", "+faststart", str(self.directory / "playback.mp4")]), 20)

    def transport(self):
        base = ports(6)
        config = self.config
        common = ["--count", str(config["duration"] * 100), "--interval-ms", "10", "--latency-ms", str(config["latency"])]
        if config["kind"] == "device":
            receiver = self.spawn("device", [self.binary("device"), "device", "--host", "127.0.0.1", "--port", str(base), *common])
            receiver.ready(3)
            for offset in range(3):
                self.proxy(f"relay-{offset}", base + 3 + offset, base + offset, "forward" if offset == 2 else "reverse")
            sender = self.spawn("operator", [self.binary("device"), "operator", "--host", "127.0.0.1", "--port", str(base + 3), *common])
        else:
            if FILTERS[config["filter"]]:
                common += ["--filter", FILTERS[config["filter"]]]
            receiver = self.spawn("receiver", [self.binary("recovery"), "receive", "--endpoint", f"127.0.0.1:{base}", *common])
            receiver.ready()
            self.proxy("relay", base + 1, base)
            sender = self.spawn("sender", [self.binary("recovery"), "send", "--endpoint", f"127.0.0.1:{base + 1}", *common])
        self.state = "running"
        self.wait(sender, config["duration"] + 15)
        self.wait(receiver, config["duration"] + 15)

    def run(self):
        try:
            if self.config["kind"] == "media":
                self.media()
            else:
                self.transport()
            self.state = "complete"
        except Exception as error:
            self.error = "" if self.stop_requested.is_set() else str(error)
            self.state = "stopped" if self.stop_requested.is_set() else "failed"
        finally:
            self.cleanup()
            (self.directory / "result.json").write_text(json.dumps(self.snapshot(), indent=2) + "\n")

    def stop(self):
        self.stop_requested.set()
        with self.lock:
            for _, process in self.children:
                if process.child.poll() is None:
                    process.child.terminate()

    def cleanup(self):
        with self.lock:
            children = list(self.children)
            decoder = self.decoder
        for _, process in children:
            process.close()
        if decoder:
            if decoder.poll() is None:
                decoder.terminate()
                try:
                    decoder.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    decoder.kill()
            decoder.wait(timeout=2)
            if self.decoder_thread:
                self.decoder_thread.join(timeout=2)
            decoder.stdout.close()

    def snapshot(self):
        observations = []
        latest = {}
        with self.lock:
            children = list(self.children)
            frames = self.decoded_frames
        for name, process in children:
            for timestamp, line in list(process.observations):
                try:
                    event = json.loads(line)
                    if isinstance(event, dict):
                        value = {**event, "process": name}
                        observations.append((timestamp, value))
                        key = (name, event.get("event"), event.get("channel"), event.get("socket"), event.get("token"))
                        latest[key] = value
                except json.JSONDecodeError:
                    pass
        observations.sort(key=lambda item: item[0])
        return {"state": self.state, "error": self.error, "config": self.config, "events": [value for _, value in observations[-200:]],
                "latest_events": list(latest.values()),
                "decoded_frames": frames, "playback": self.state == "complete" and (self.directory / "playback.mp4").exists(),
                "paused": [p.exists() for p in self.markers], "directory": str(self.directory)}


class Console:
    def __init__(self, args):
        self.args = args
        self.token = secrets.token_urlsafe(32)
        self.job = None
        self.lock = threading.Lock()

    def start(self, config):
        with self.lock:
            if self.job and self.job.thread.is_alive():
                raise ValueError("a run is already active")
            self.job = Job(self.args, configuration(config))
            self.job.thread.start()


class ConsoleServer(ThreadingHTTPServer):
    def server_bind(self):
        # The numeric loopback endpoint needs no potentially blocking reverse DNS.
        TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


class Handler(BaseHTTPRequestHandler):
    server_version = "SRTSampleConsole/1"

    def handle(self):
        self.connection.settimeout(5)
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            # Seeking a video or navigating away cancels an in-flight response.
            pass

    def log_message(self, *_):
        pass  # Never log cookies or the bootstrap token.

    def send(self, code, body=b"", content_type="application/json", headers=None):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; img-src 'self' blob:; media-src 'self'; frame-ancestors 'none'")
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        expected = self.server.console.token
        token = self.headers.get("Authorization", "").removeprefix("Bearer ")
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get("Cookie", ""))
            if not token and "srt_sample_session" in cookie:
                token = cookie["srt_sample_session"].value
        except Exception:
            return False
        return hmac.compare_digest(token.encode("utf-8"), expected.encode("ascii"))

    def valid_host(self):
        return self.headers.get("Host") in (f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}")

    def do_GET(self):
        if not self.valid_host():
            return self.send(403)
        route = urlsplit(self.path).path
        static = {"/": "index.html", "/app.js": "app.js", "/style.css": "style.css"}
        if route in static:
            file = ROOT / "web" / static[route]
            return self.send(200, file.read_bytes(), mimetypes.guess_type(file.name)[0] or "text/plain")
        if not self.authorized():
            return self.send(401, b'{"error":"open the launch URL to authenticate"}')
        console = self.server.console
        job = console.job
        if route == "/api/state":
            result = job.snapshot() if job else {"state": "idle", "events": [], "decoded_frames": 0}
            result["capabilities"] = {"ffmpeg": bool(console.args.ffmpeg), "file": bool(console.args.media)}
            return self.send(200, json.dumps(result).encode())
        if route == "/api/frame":
            with job.lock if job else threading.Lock():
                frame = job.jpeg if job else b""
            return self.send(200 if frame else 204, frame, "image/jpeg")
        if route == "/api/playback" and job and job.state == "complete":
            file = job.directory / "playback.mp4"
            if not file.exists():
                return self.send(404)
            size = file.stat().st_size
            start, end, code = 0, size - 1, 200
            if self.headers.get("Range"):
                match = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers["Range"])
                if not match:
                    return self.send(416)
                start = int(match[1])
                end = min(int(match[2]) if match[2] else size - 1, size - 1)
                if start > end:
                    return self.send(416)
                code = 206
            self.send_response(code)
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Cache-Control", "no-store")
            if code == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            with file.open("rb") as source:
                source.seek(start)
                remaining = end - start + 1
                while remaining:
                    data = source.read(min(65536, remaining))
                    if not data:
                        break
                    self.wfile.write(data)
                    remaining -= len(data)
            return
        return self.send(404)

    def do_POST(self):
        origin = self.headers.get("Origin")
        if not self.valid_host() or (origin and origin not in (f"http://127.0.0.1:{self.server.server_port}", f"http://localhost:{self.server.server_port}")):
            return self.send(403)
        if not self.authorized():
            return self.send(401)
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 <= size <= 4096:
                raise ValueError("request too large")
            data = json.loads(self.rfile.read(size) or b"{}")
            console = self.server.console
            if self.path == "/api/session":
                return self.send(200, b"{}", headers={"Set-Cookie": f"srt_sample_session={console.token}; HttpOnly; SameSite=Strict; Path=/"})
            if self.path == "/api/start":
                console.start(data)
            elif self.path == "/api/stop":
                if console.job:
                    console.job.stop()
            elif self.path == "/api/path":
                if not isinstance(data, dict) or type(data.get("index")) is not int or data["index"] not in (0, 1) or type(data.get("paused")) is not bool:
                    raise ValueError("invalid path control")
                with console.lock:
                    job = console.job
                    if not job or job.config["kind"] != "media" or job.state != "running":
                        raise ValueError("start a media run first")
                    marker = job.markers[data["index"]]
                    if data["paused"]:
                        marker.touch()
                    else:
                        marker.unlink(missing_ok=True)
            else:
                return self.send(404)
            self.send(200, b"{}")
        except (ValueError, TypeError) as error:
            self.send(400, json.dumps({"error": str(error)}).encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg"))
    parser.add_argument("--media", type=Path, help="optional user-selected source file exposed to the control panel")
    parser.add_argument("--output-dir", type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument("--port", type=int, default=8787)
    args = parser.parse_args()
    args.binary_dir = args.binary_dir.resolve()
    if args.media:
        args.media = args.media.resolve(strict=True)
    if args.ffmpeg:
        args.ffmpeg = str(Path(args.ffmpeg).resolve(strict=True))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    server = ConsoleServer(("127.0.0.1", args.port), Handler)
    server.console = Console(args)
    print(f"http://127.0.0.1:{server.server_port}/#{server.console.token}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if server.console.job:
            server.console.job.stop()
            server.console.job.thread.join(timeout=15)
        server.server_close()


if __name__ == "__main__":
    main()
