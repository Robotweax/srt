#!/usr/bin/env python3
"""Prepare and compare identical same-host UDP group peers on two clean revisions.

Explicitly a fixed-workload diagnostic, not a general capacity certification.
No downloads, installation, kernel tuning or speed thresholds.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import queue
import resource
import shlex
import shutil
import statistics
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def identity(path):
    return {"path": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def prepare(args, out):
    if platform.system() not in ("Darwin", "Linux"):
        raise ValueError("the peer currently requires POSIX/macOS")
    manifest = {"complete": False, "sources": {}, "programs": {}, "libraries": {},
                "stream_profile": "live-continuation-v2", "periodic_nak": True,
                "commands": [], "platform": platform.platform(), "architecture": platform.machine(),
                "harness": identity(ROOT / "benchmarks/group_throughput_peer.cpp")}
    path = out / "build-manifest.json"

    def run(command, name):
        manifest["commands"].append(command)
        write_json(path, manifest)
        with (out / f"{name}.log").open("w") as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        return (out / f"{name}.log").read_text()

    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise ValueError("C++ compiler not found")
    try:
        run([compiler, "--version"], "compiler")
        run(["cmake", "--version"], "cmake")
        flags = shlex.split(run(["pkg-config", "--cflags", "--libs", "libcrypto"], "crypto"))
        run(["openssl", "version", "-a"], "openssl")
        for label, source in (("before", args.before_source), ("after", args.after_source)):
            source = source.resolve(strict=True)
            revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
            status = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True)
            if status:
                raise ValueError(f"dirty source tree: {source}")
            manifest["sources"][label] = {"path": str(source), "revision": revision}
            build = out / f"{label}-build"
            run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                 "-DCMAKE_CXX_FLAGS=-g", "-DBUILD_SHARED_LIBS=OFF",
                 "-DROBOTWEAX_SRT_CRYPTO_BACKEND=openssl", "-DROBOTWEAX_SRT_BUILD_TESTS=OFF",
                 "-DROBOTWEAX_SRT_BUILD_TOOLS=OFF", "-DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF",
                 "-DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF", f"-DCMAKE_CXX_COMPILER={compiler}"], f"{label}-configure")
            run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], f"{label}-build")
            library = build / "librobotweax-srt.a"
            program = out / f"{label}-peer"
            command = [compiler, "-std=c++17", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                       str(ROOT / "benchmarks/group_throughput_peer.cpp"), f"-I{source / 'include'}",
                       str(library), *flags, "-pthread", "-o", str(program)]
            if platform.system() == "Linux":
                command.append("-ldl")
            run(command, f"{label}-peer")
            manifest["programs"][label] = identity(program)
            manifest["libraries"][label] = identity(library)
            shutil.copyfile(build / "CMakeCache.txt", out / f"{label}-cache.txt")
            run(["otool", "-L", str(program)] if platform.system() == "Darwin" else ["ldd", str(program)], f"{label}-loader")
        manifest["complete"] = True
        return manifest
    finally:
        write_json(path, manifest)


class Peer:
    def __init__(self, command, log_prefix):
        self.events = queue.Queue()
        self.stderr = log_prefix.with_suffix(".stderr").open("w")
        self.stdout = log_prefix.with_suffix(".stdout").open("w")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.stderr, text=True, bufsize=1)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        try:
            for line in self.process.stdout:
                self.stdout.write(line)
                self.stdout.flush()
                try:
                    self.events.put(json.loads(line))
                except ValueError:
                    self.events.put({"event": "invalid", "line": line})
        finally:
            self.events.put({"event": "eof"})

    def expect(self, event, deadline):
        try:
            value = self.events.get(timeout=max(0, deadline - time.monotonic()))
        except queue.Empty as error:
            raise RuntimeError(f"timeout waiting for {event}") from error
        if value.get("event") != event:
            raise RuntimeError(f"expected {event}, got {value}")
        return value

    def go(self, command="go"):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait()
        self.reader.join(timeout=2)
        self.process.stdin.close()
        self.process.stdout.close()
        self.stdout.close()
        self.stderr.close()


def validate_result(sender, receiver, members, messages, size, mode):
    for value in (sender, receiver):
        if (value["members"], value["messages"], value["useful_bytes"]) != (members, messages, messages * size):
            raise ValueError("result count mismatch")
        if len(value["member_stats"]) != members:
            raise ValueError("member statistics missing")
        for key in ("seconds", "cpu_seconds", "latency_us_p50", "latency_us_p99"):
            if not math.isfinite(value[key]) or value[key] < 0 or (key == "seconds" and value[key] == 0):
                raise ValueError("invalid timing")
        if len(value["sha256"]) != 64 or any(c not in "0123456789abcdef" for c in value["sha256"]):
            raise ValueError("invalid digest")
        for stats in value["member_stats"]:
            if any(not isinstance(n, int) or n < 0 for n in stats.values()):
                raise ValueError("invalid transport counters")
            if stats["send_drops"] or stats["receive_drops"]:
                raise ValueError("transport drop invalidates qualification")
    if sender["sha256"] != receiver["sha256"]:
        raise ValueError("sender/receiver payload SHA-256 mismatch")
    if any(value.get("stream_profile") != "live-continuation-v2" for value in (sender, receiver)):
        raise ValueError("unexpected stream profile")
    for value in (sender, receiver):
        limit = 4096 if value is sender else 8192
        continuation = value.get("continuation_messages")
        if not isinstance(continuation, int) or not 0 <= continuation <= limit:
            raise ValueError("invalid continuation budget")
    copies = []
    for stats in sender["member_stats"]:
        payload = stats["payload_queued_packets"]
        continuation = stats["continuation_queued_packets"]
        previous = stats["previous_continuation_queued_packets"]
        if (payload > messages or continuation > sender["continuation_messages"]
                or previous > 4096 or not payload <= stats["unique_sent_packets"] <= payload + continuation + previous):
            raise ValueError("invalid sent copy accounting")
        copies.append(payload)
    if sum(copies) < messages or (mode == "broadcast" and any(n != messages for n in copies)):
        raise ValueError("missing expected sent copies")
    span = receiver["delivery_span_seconds"]
    if not math.isfinite(span) or span < 0 or (messages > 1 and span <= 0):
        raise ValueError("invalid delivery span")
    seconds = max(sender["seconds"], receiver["seconds"])
    return {"delivery_span_mbps": (messages - 1) * size * 8 / span / 1000000 if span > 0 else None,
            "useful_mbps": messages * size * 8 / seconds / 1000000,
            "cpu_seconds": sender["cpu_seconds"] + receiver["cpu_seconds"],
            "delivered_latency_us_p99": receiver["latency_us_p99"],
            "sent_packets": sum(s["sent_packets"] for s in sender["member_stats"]),
            "retransmitted_packets": sum(s["retransmitted_packets"] for s in sender["member_stats"])}


def case(program, mode, members, messages, size, log_prefix, timeout, relay_factory=None):
    peers = []
    relay = None
    deadline = time.monotonic() + timeout
    base = [mode, "0", str(members), str(messages), str(size)]
    try:
        receiver = Peer([str(program), "receive", *base], log_prefix.with_name(log_prefix.name + "-receive"))
        peers.append(receiver)
        port = receiver.expect("listening", deadline)["port"]
        if relay_factory is not None:
            relay = relay_factory(port)
            port = relay.port
        base[1] = str(port)
        sender = Peer([str(program), "send", *base], log_prefix.with_name(log_prefix.name + "-send"))
        peers.append(sender)
        for peer in reversed(peers):
            peer.expect("ready", deadline)
        for peer in peers:
            peer.go()
        receiver.expect("warm", deadline)
        sender.go("stop")
        sender.expect("warm", deadline)
        for peer in peers:
            peer.go()
        for peer in reversed(peers):
            peer.expect("armed", deadline)
        for peer in peers:
            peer.go()
        receive_result = receiver.expect("result", deadline)
        sender.go("stop")
        send_result = sender.expect("result", deadline)
        metrics = validate_result(send_result, receive_result, members, messages, size, mode)
        for peer in peers:
            peer.go()
        for peer in peers:
            if peer.process.wait(timeout=max(0.001, deadline - time.monotonic())) != 0:
                raise RuntimeError("peer exited unsuccessfully")
        result = {"qualified": True, "sender": send_result, "receiver": receive_result, "metrics": metrics}
        if relay is not None:
            result["forced_tail_losses"] = relay.validate(members)
        return result
    finally:
        for peer in peers:
            peer.close()
        if relay is not None:
            relay.close()


def summarize(cases):
    summary = []
    keys = sorted({(c["mode"], c["members"], c["size"]) for c in cases})
    for mode, members, size in keys:
        selected = [c for c in cases if (c["mode"], c["members"], c["size"]) == (mode, members, size)]
        entry = {"mode": mode, "members": members, "size": size, "all_qualified": all(c["qualified"] for c in selected)}
        for label in ("before", "after"):
            valid = [c["metrics"] for c in selected if c["label"] == label and c["qualified"]]
            entry[label] = {"qualified_runs": len(valid)}
            if valid:
                for metric in ("useful_mbps", "cpu_seconds", "delivered_latency_us_p99", "delivery_span_mbps"):
                    values = [c[metric] for c in valid if c[metric] is not None]
                    if not values:
                        continue
                    entry[label][metric] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        summary.append(entry)
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before-source", type=Path)
    parser.add_argument("--after-source", type=Path)
    parser.add_argument("--prepared-directory", type=Path, help="reuse verified manifest/binaries")
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--members", default="2,8,16,32,64")
    parser.add_argument("--sizes", default="1316")
    parser.add_argument("--messages", type=int, default=10000)
    parser.add_argument("--iterations", type=int, default=3)
    parser.add_argument("--timeout-seconds", type=float, default=90)
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=2)
    args = parser.parse_args(argv)
    counts = [int(n) for n in args.members.split(",")]
    sizes = [int(n) for n in args.sizes.split(",")]
    if not counts or any(n < 1 or n > 64 for n in counts) or not sizes or any(n < 16 or n > 1316 for n in sizes):
        parser.error("members must be 1..64 and sizes 16..1316")
    if not 1 <= args.messages <= 10000000 or args.iterations < 1 or not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0:
        parser.error("invalid run budget")
    if not args.prepared_directory and (not args.before_source or not args.after_source):
        parser.error("provide both sources or a prepared directory")
    out = args.output_directory.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"schema_version": 2, "complete": False, "cases": [], "platform": platform.platform(),
              "settings": {"members": counts, "sizes": sizes, "messages": args.messages, "iterations": args.iterations,
                           "stream_profile": "live-continuation-v2", "periodic_nak": True,
                           "continuation_limit": 4096, "latency_ms": 120, "pending_packets_per_member": 64, "warmup_messages": 256,
                           "encryption": "none", "host": "loopback", "timeout_seconds": args.timeout_seconds},
              "controller": identity(Path(__file__)), "architecture": platform.machine(),
              "fd_limit": list(resource.getrlimit(resource.RLIMIT_NOFILE))}
    try:
        if args.prepared_directory:
            manifest = json.loads((args.prepared_directory / "build-manifest.json").read_text())
        else:
            manifest = prepare(args, out)
        if manifest.get("platform") != platform.platform() or manifest.get("architecture") != platform.machine():
            raise ValueError("prepared manifest belongs to a different host platform")
        if not manifest["complete"]:
            raise ValueError("incomplete build manifest")
        if manifest.get("stream_profile") != "live-continuation-v2" or manifest.get("periodic_nak") is not True:
            raise ValueError("prepared manifest uses a different Live stream profile")
        if manifest["harness"]["sha256"] != identity(ROOT / "benchmarks/group_throughput_peer.cpp")["sha256"]:
            raise ValueError("prepared harness differs from current source")
        for label in ("before", "after"):
            for section in ("programs", "libraries"):
                expected = manifest[section][label]
                if identity(Path(expected["path"]))["sha256"] != expected["sha256"]:
                    raise ValueError("prepared binary/library identity mismatch")
        report["build_manifest"] = manifest
        for mode in ("broadcast", "backup"):
            for count in counts:
                for size in sizes:
                    for iteration in range(args.iterations):
                        # Alternate serial A/B order to reduce simple host/order bias.
                        labels = ("before", "after") if iteration % 2 == 0 else ("after", "before")
                        for label in labels:
                            key = f"{mode}-{count}-{size}-{iteration}-{label}"
                            result = {"mode": mode, "members": count, "size": size, "iteration": iteration, "label": label, "load_average": list(os.getloadavg())}
                            try:
                                result.update(case(Path(manifest["programs"][label]["path"]), mode, count,
                                                   args.messages, size, out / key, args.timeout_seconds))
                            except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
                                result.update(qualified=False, error=str(error))
                            report["cases"].append(result)
                            report["summary"] = summarize(report["cases"])
                            write_json(out / "report.json", report)
                            print(json.dumps({"case": key, "qualified": result["qualified"], "metrics": result.get("metrics"), "error": result.get("error")}), flush=True)
        report["complete"] = True
        return 0 if all(c["qualified"] for c in report["cases"]) else 1
    finally:
        write_json(out / "report.json", report)


if __name__ == "__main__":
    raise SystemExit(main())
