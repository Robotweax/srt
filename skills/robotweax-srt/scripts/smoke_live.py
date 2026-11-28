#!/usr/bin/env python3
"""Qualify clear IPv4 Live/Message on loopback with synthetic MPEG-TS A/V."""

import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import platform
import re
import signal
import socket
import subprocess
import sys
import time

from verify_artifact import sha256

HOST = "127.0.0.1"
PAYLOAD = 1316
LATENCY_MS = 120
TS_NULL = (b"\x47\x1f\xff\x10" + b"\xff" * 184)


def run(command):
    return subprocess.run(command, check=True, text=True, capture_output=True, timeout=25).stdout


def frame_hashes(decoder, media, track):
    # The data column contains frame/sample hashes. PTS changes can be measured
    # separately; this fixture checks complete decoded content in order.
    output = run([decoder, "-v", "error", "-xerror", "-i", media,
                  "-map", f"0:{track}:0", "-f", "framehash", "-"])
    return [line.split(",")[-1].strip() for line in output.splitlines()
            if line and not line.startswith("#")]


def udp_socket(stack):
    sock = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
    sock.bind((HOST, 0))
    return sock


def peer_command(args, direction, role, ports):
    ingest, playback, srt = ports
    udp = ingest if direction == "send" else playback
    srt_uri = f"srt://{HOST}:{srt}?mode={role}"
    udp_uri = f"udp://{HOST}:{udp}"
    if args.peer == "gstreamer":
        gst = [args.peer_bin, "-q", "-e"]
        if direction == "send":
            return gst + ["udpsrc", f"address={HOST}", f"port={udp}", "!",
                          "srtsink", f"uri={srt_uri}", f"latency={LATENCY_MS}"]
        return gst + ["srtsrc", f"uri={srt_uri}", f"latency={LATENCY_MS}", "!",
                      "udpsink", f"host={HOST}", f"port={udp}", "sync=false"]
    if args.peer == "haivision":
        srt_uri += f"&latency={LATENCY_MS}&payloadsize={PAYLOAD}"
        endpoints = (udp_uri, srt_uri) if direction == "send" else (srt_uri, udp_uri)
        return [args.peer_bin, "-v", "-a:no", "-c:1316", *endpoints]
    srt_uri += f"&latency={LATENCY_MS * 1000}&payload_size={PAYLOAD}"
    udp_uri += f"?pkt_size={PAYLOAD}"
    source, destination = (udp_uri + "&timeout=2000000", srt_uri) if direction == "send" else (srt_uri, udp_uri)
    return [args.peer_bin, "-nostdin", "-loglevel", "info", "-f", "data",
            "-raw_packet_size", str(PAYLOAD), "-probesize", "32", "-analyzeduration", "1",
            "-i", source, "-map", "0:0", "-c", "copy", "-flush_packets", "1",
            "-f", "data", destination]


def bridge_command(args, direction, role, ports):
    ingest, playback, srt = ports
    return [args.bridge, direction, "--srt-mode", role, "--srt-host", HOST,
            "--srt-port", str(srt), "--udp-host", HOST,
            "--udp-port", str(ingest if direction == "send" else playback),
            "--latency-ms", str(LATENCY_MS), "--connect-timeout-ms", "5000"]


def start(command, log, stack):
    # Keep ambient loader overrides out of the test; rely on the artifact's
    # deployed search configuration. The loader log identifies loaded paths.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("DYLD_", "LD_"))}
    env["LC_ALL"] = "C"
    env.update({"DYLD_PRINT_LIBRARIES": "1"} if sys.platform == "darwin" else {"LD_DEBUG": "libs"})
    output = stack.enter_context(log.open("wb"))
    process = subprocess.Popen(command, stdout=output, stderr=output, env=env)
    stack.callback(stop, process)
    return process


def interrupt(process):
    if process.poll() is None:
        process.send_signal(signal.SIGINT)


def finish(process):
    try:
        return process.wait(timeout=4)
    except subprocess.TimeoutExpired:
        process.kill()
        return process.wait()


def stop(process):
    interrupt(process)
    return finish(process)


def wait_for(log, marker, process):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        if marker in log.read_text(errors="replace"):
            return
        if process.poll() is not None:
            raise RuntimeError(f"{log.name} exited {process.returncode} before {marker}")
        time.sleep(0.03)
    raise RuntimeError(f"{log.name} timed out waiting for {marker}")


def loaded_provider(log, process, expected):
    paths = set()
    for line in log.read_text(errors="replace").splitlines():
        if sys.platform == "darwin":
            match = re.match(rf"^dyld\[{process.pid}\]: (?:<[^>]+> )?(/.+)$", line)
        else:
            match = re.match(rf"^\s*{process.pid}:\s+calling init:\s+(/.+)$", line)
        if match:
            path = Path(match[1].strip())
            if re.match(r"lib(?:robotweax-)?srt(?:[.-]|$)", path.name):
                paths.add(path.resolve(strict=True))
    if len(paths) != 1:
        raise RuntimeError(f"{log.name}: expected one SRT shared image, found {sorted(map(str, paths))}")
    loaded = paths.pop()
    actual, trusted = sha256(loaded), sha256(Path(expected))
    if actual != trusted:
        raise RuntimeError(f"{log.name}: loaded {loaded} ({actual}), expected {expected} ({trusted})")
    return {"path": str(loaded), "sha256": actual}


def trial_error(error, directory, ports):
    for name in ("robotweax.log", "peer.log"):
        log = directory / name
        if not log.is_file():
            continue
        for line in log.read_text(errors="replace").splitlines():
            if re.search(
                r"address already in use|\bEADDRINUSE\b|\bsrt_bind:|\bcannot bind to\b",
                line,
                re.IGNORECASE,
            ):
                ingest, playback, srt = ports
                return (
                    f"loopback UDP bind failure (ingest={ingest}, playback={playback}, SRT={srt}): "
                    f"{name}: {line.strip()}"
                )
    return str(error)


def relay_media(tx, rx, ingest, fixture, received):
    fixture += TS_NULL * ((-len(fixture) % PAYLOAD) // len(TS_NULL))
    packets = [fixture[index:index + PAYLOAD] for index in range(0, len(fixture), PAYLOAD)]
    rx.setblocking(False)
    with received.open("wb") as capture:
        def collect():
            while True:
                try:
                    capture.write(rx.recv(65535))
                except BlockingIOError:
                    return

        for packet in packets:
            tx.sendto(packet, (HOST, ingest))
            time.sleep(0.003)
            collect()
        deadline = time.monotonic() + 0.8
        while time.monotonic() < deadline:
            time.sleep(0.02)
            collect()


def trial(args, ports, tx, rx, fixture, expected_frames, directory, result):
    direction, role = result["robotweax_direction"], result["robotweax_role"]
    peer_direction = "receive" if direction == "send" else "send"
    peer_role = "listener" if role == "caller" else "caller"
    commands = {"robotweax": bridge_command(args, direction, role, ports),
                "peer": peer_command(args, peer_direction, peer_role, ports)}
    result["commands"] = commands
    listener = "robotweax" if role == "listener" else "peer"
    caller = "peer" if listener == "robotweax" else "robotweax"
    with ExitStack() as stack:
        logs = {name: directory / f"{name}.log" for name in commands}
        procs = {listener: start(commands[listener], logs[listener], stack)}
        if listener == "robotweax":
            wait_for(logs[listener], "READY SRT listener", procs[listener])
        else:
            # External adapters expose different readiness APIs. A bounded
            # startup window plus a connected/media gate detects a failed bind.
            time.sleep(0.45)
        procs[caller] = start(commands[caller], logs[caller], stack)
        ffmpeg_sends = args.peer == "ffmpeg" and peer_direction == "send"
        if ffmpeg_sends:
            # FFmpeg needs input before opening its SRT output. Prime it with
            # null packets, which do not contribute any decoded frames.
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                tx.sendto(TS_NULL * 7, (HOST, ports[0]))
                if "CONNECTED direction=" in logs["robotweax"].read_text(errors="replace"):
                    break
                if any(proc.poll() is not None for proc in procs.values()):
                    break
                time.sleep(0.04)
        wait_for(logs["robotweax"], "CONNECTED direction=", procs["robotweax"])
        time.sleep(0.2)
        result["providers"] = {
            name: loaded_provider(logs[name], procs[name], expected)
            for name, expected in (("robotweax", args.robotweax_library),
                                   ("peer", args.peer_library))
        }
        received = directory / "received.ts"
        relay_media(tx, rx, ports[0], fixture, received)
        # Ask both processes to close before waiting. Either endpoint can
        # otherwise exit through its remote-disconnect path first.
        receiver = "robotweax" if direction == "receive" else "peer"
        sender = "peer" if receiver == "robotweax" else "robotweax"
        interrupt(procs[receiver])
        interrupt(procs[sender])
        result["exit_codes"] = {name: finish(proc) for name, proc in procs.items()}
    result["streams"] = {}
    for track, kind in (("v", "video"), ("a", "audio")):
        observed = frame_hashes(args.decoder, received, track)
        result["streams"][kind] = {"expected_frames": len(expected_frames[track]),
                                   "decoded_frames": len(observed), "match": observed == expected_frames[track]}
        if not observed or observed != expected_frames[track]:
            raise RuntimeError(f"decoded {kind} differs from fixture")
    result["received_sha256"] = sha256(received)
    peer_exit = {0, 255} if args.peer == "ffmpeg" else {0}
    if result["exit_codes"]["robotweax"] != 0 or result["exit_codes"]["peer"] not in peer_exit:
        raise RuntimeError(f"unexpected shutdown exit codes: {result['exit_codes']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge", type=Path, required=True)
    parser.add_argument("--robotweax-library", type=Path, required=True)
    parser.add_argument("--robotweax-build", required=True)
    parser.add_argument("--peer", choices=("gstreamer", "haivision", "ffmpeg"), required=True)
    parser.add_argument("--peer-bin", type=Path, required=True,
                        help="actual peer executable for same-PID loader verification")
    parser.add_argument("--peer-library", type=Path, required=True)
    parser.add_argument("--peer-build", required=True)
    parser.add_argument("--decoder", type=Path, required=True, help="FFmpeg with MPEG-2 and MP2")
    parser.add_argument("--output", type=Path, required=True, help="new evidence directory")
    args = parser.parse_args()
    if sys.platform not in ("darwin", "linux"):
        parser.error("the loader evidence collector supports Linux and macOS")
    for attr in ("bridge", "robotweax_library", "peer_bin", "peer_library", "decoder"):
        setattr(args, attr, str(getattr(args, attr).absolute()))
    output = args.output.resolve()
    output.mkdir()
    report = {"os": platform.platform(), "robotweax_build": args.robotweax_build,
              "peer": args.peer, "peer_build": args.peer_build, "options": {
                  "address_family": "IPv4", "mode": "Live/Message", "cipher": "clear",
                  "latency_ms": LATENCY_MS, "max_udp_payload_bytes": PAYLOAD},
              "executables": {}, "results": []}
    try:
        report["executables"] = {name: {"path": path, "sha256": sha256(Path(path))}
                                 for name, path in (("bridge", args.bridge), ("peer", args.peer_bin),
                                                    ("decoder", args.decoder))}
        fixture = output / "fixture.ts"
        fixture_command = [args.decoder, "-nostdin", "-v", "error",
                           "-f", "lavfi", "-i", "testsrc2=size=160x96:rate=25",
                           "-f", "lavfi", "-i", "sine=frequency=997:sample_rate=48000",
                           "-t", "3", "-map", "0:v:0", "-map", "1:a:0", "-c:v", "mpeg2video",
                           "-bf", "0", "-c:a", "mp2", "-b:a", "128k", "-f", "mpegts", str(fixture)]
        report["fixture_command"] = fixture_command
        run(fixture_command)
        report["fixture_sha256"] = sha256(fixture)
        expected = {track: frame_hashes(args.decoder, fixture, track) for track in ("v", "a")}
        fixture_bytes = fixture.read_bytes()
        for direction in ("send", "receive"):
            for role in ("caller", "listener"):
                with ExitStack() as sockets:
                    tx, rx = udp_socket(sockets), udp_socket(sockets)
                    with ExitStack() as reservations:
                        reserved = [udp_socket(reservations).getsockname()[1] for _ in range(2)]
                    ports = reserved[0], rx.getsockname()[1], reserved[1]
                    # Repeat with identical endpoints after both processes close.
                    for session in (1, 2):
                        directory = output / f"{direction}-{role}-{session}"
                        directory.mkdir()
                        result = {"robotweax_direction": direction, "robotweax_role": role,
                                  "peer_direction": "receive" if direction == "send" else "send",
                                  "peer_role": "listener" if role == "caller" else "caller",
                                  "session": session, "status": "FAIL"}
                        report["results"].append(result)
                        try:
                            trial(args, ports, tx, rx, fixture_bytes, expected, directory, result)
                            result["status"] = "PASS"
                        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                            result["error"] = trial_error(error, directory, ports)
                            if isinstance(error, subprocess.CalledProcessError):
                                (directory / "media-error.log").write_text(error.stderr or "")
                        rx.setblocking(False)
                        while True:
                            try:
                                rx.recv(65535)
                            except BlockingIOError:
                                break
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "results.json")
    return 0 if all(row["status"] == "PASS" for row in report["results"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
