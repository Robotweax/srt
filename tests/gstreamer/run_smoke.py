#!/usr/bin/env python3
"""Build an installed-API peer and qualify the actual SRT plugin/provider pair."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import errno
import os
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import tempfile
import time


def run(command, *, env=None):
    return subprocess.run(
        command,
        env=env,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=60,
    ).stdout


def provider(prefix: Path, srt_prefix: Path, artifacts: Path, name: str):
    prefix, srt_prefix = prefix.resolve(), srt_prefix.resolve()
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("GST_", "LD_", "DYLD_"))
    }
    plugins = prefix / "lib/gstreamer-1.0"
    plugin = plugins / (
        "libgstsrt.dylib" if sys.platform == "darwin" else "libgstsrt.so"
    )
    if not plugin.is_file():
        raise RuntimeError(f"missing SRT plugin: {plugin}")
    env.update(
        {
            "GST_PLUGIN_SYSTEM_PATH_1_0": "",
            "GST_PLUGIN_PATH_1_0": str(plugins),
            "GST_REGISTRY_1_0": str(artifacts / f"{name}.registry"),
            "GST_PLUGIN_SCANNER_1_0": str(
                prefix / "libexec/gstreamer-1.0/gst-plugin-scanner"
            ),
            "ROBOTWEAX_GST_PLUGIN": str(plugin),
            "ROBOTWEAX_SRT_LIBRARY_DIR": str((srt_prefix / "lib").resolve()),
            "PKG_CONFIG_PATH": str(prefix / "lib/pkgconfig"),
            "LC_ALL": "C",
        }
    )
    for element in ("srtsrc", "srtsink"):
        description = run([str(prefix / "bin/gst-inspect-1.0"), element], env=env)
        if str(plugin) not in description:
            raise RuntimeError(f"{element} was not loaded from {plugin}")
        (artifacts / f"{name}-{element}.txt").write_text(description)
    packages = ["gstreamer-app-1.0", "gmodule-2.0"]
    flags = shlex.split(run(["pkg-config", "--cflags", "--libs", *packages], env=env))
    peer = artifacts / f"peer-{name}"
    command = [
        *shlex.split(os.environ.get("CC", "cc")),
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        str(Path(__file__).with_name("peer.c")),
        "-o",
        str(peer),
        *flags,
        f"-Wl,-rpath,{prefix / 'lib'}",
    ]
    if sys.platform.startswith("linux"):
        command.append("-ldl")
    run(command, env=env)
    return peer, env


def port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def uri(number, mode, encrypted=False, *, ffmpeg=False, streamid="robotweax-test"):
    query = f"mode={mode}&streamid={streamid}"
    if ffmpeg:
        query += (
            "&latency=80000&payload_size=1316&linger=2"
            "&timeout=5000000&connect_timeout=1500"
        )
        if mode == "listener":
            query += "&listen_timeout=5000000"
    else:
        query += "&latency=80&payloadsize=1316&conntimeo=1500"
    if encrypted:
        # Public fixture secret, never a deployment credential.
        query += "&passphrase=robotweax-gstreamer-test&pbkeylen=16"
    return f"srt://127.0.0.1:{number}?{query}"


@contextmanager
def process(command, env, log):
    with log.open("w") as output:
        child = subprocess.Popen(
            command,
            env=env,
            stdin=subprocess.PIPE,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
        try:
            yield child
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
            child.stdin.close()


def wait_listener(child, number):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise RuntimeError("listener exited before binding")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            try:
                probe.bind(("127.0.0.1", number))
            except OSError as error:
                if error.errno == errno.EADDRINUSE:
                    return
                raise
        time.sleep(0.02)
    raise RuntimeError("listener did not bind within five seconds")


def require_success(child, label):
    status = child.wait(timeout=25)
    if status != 0:
        raise RuntimeError(f"{label} failed with status {status}")


def gst_command(provider, sending, url, file, expected=0, repeat=False):
    peer, env = provider
    return (
        [
            str(peer),
            "send" if sending else "receive-repeat" if repeat else "receive",
            url,
            str(file),
            str(expected),
        ],
        env,
    )


def release_sender(child):
    child.stdin.write(b"\n")
    child.stdin.flush()


def pair(
    sender,
    receiver,
    artifacts,
    label,
    number,
    sink_listener=False,
    ffmpeg_sender=False,
    ffmpeg_receiver=False,
):
    first, second = (sender, receiver) if sink_listener else (receiver, sender)
    with process(*first, artifacts / f"{label}-listener.log") as listener:
        wait_listener(listener, number)
        with process(*second, artifacts / f"{label}-caller.log") as caller:
            tx, rx = (listener, caller) if sink_listener else (caller, listener)
            if ffmpeg_receiver:
                # FFmpeg needs transport EOF to flush its final demux packet.
                # This explicit loopback drain allowance at 80-ms latency is
                # not a general remote-delivery guarantee.
                sender_log = (
                    artifacts
                    / f"{label}-{'listener' if sink_listener else 'caller'}.log"
                )
                deadline = time.monotonic() + 20
                while "DONE bytes=" not in sender_log.read_text():
                    if tx.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("sender did not reach EOS")
                    time.sleep(0.02)
                time.sleep(0.5)
                release_sender(tx)
            require_success(rx, label + " receiver")
            if not ffmpeg_sender:
                if not ffmpeg_receiver:
                    release_sender(tx)
                require_success(tx, label + " sender")


def extract(ffmpeg, source, target):
    run(
        [
            str(ffmpeg),
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(source),
            "-map",
            "0:v:0",
            "-c",
            "copy",
            "-f",
            "data",
            str(target),
        ]
    )
    return target.read_bytes()


def qualify(args, artifacts):
    robot = provider(args.gst_prefix, args.srt_prefix, artifacts, "robotweax")
    reference = None
    if args.reference_gst_prefix:
        reference = provider(
            args.reference_gst_prefix, args.reference_srt_prefix, artifacts, "haivision"
        )
    ffmpeg = args.ffmpeg.resolve()
    fixture = artifacts / "fixture.ts"
    run(
        [
            str(ffmpeg),
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=160x90:rate=25",
            "-t",
            "1.2",
            "-c:v",
            "mpeg2video",
            "-g",
            "12",
            "-f",
            "mpegts",
            str(fixture),
        ]
    )
    payload = fixture.read_bytes()
    if not payload or len(payload) % 188:
        raise RuntimeError("fixture is not a nonempty MPEG-TS sequence")
    expected_video = extract(ffmpeg, fixture, artifacts / "expected.m2v")
    if not expected_video:
        raise RuntimeError("fixture has no encoded video")
    remuxed = artifacts / "remuxed.ts"
    run(
        [
            str(ffmpeg),
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(fixture),
            "-map",
            "0:v:0",
            "-c",
            "copy",
            "-f",
            "mpegts",
            str(remuxed),
        ]
    )

    routes = [("robotweax", robot, robot)]
    if reference:
        routes += [
            ("robotweax-to-haivision", robot, reference),
            ("haivision-to-robotweax", reference, robot),
        ]
    for route, tx, rx in routes:
        for encrypted in (False, True):
            for sink_listener in (False, True):
                label = (
                    f"{route}-{'aes' if encrypted else 'clear'}-"
                    f"{'sink' if sink_listener else 'src'}-listener"
                )
                number = port()
                received = artifacts / f"{label}.ts"
                sender = gst_command(
                    tx,
                    True,
                    uri(number, "listener" if sink_listener else "caller", encrypted),
                    fixture,
                )
                receiver = gst_command(
                    rx,
                    False,
                    uri(number, "caller" if sink_listener else "listener", encrypted),
                    received,
                    len(payload),
                )
                pair(sender, receiver, artifacts, label, number, sink_listener)
                if received.read_bytes() != payload:
                    raise RuntimeError(f"{label}: MPEG-TS bytes differ")
                print(f"PASS {label}", flush=True)

    for sending in (False, True):
        for encrypted in (False, True):
            label = (
                f"{'gst-to-ffmpeg' if sending else 'ffmpeg-to-gst'}-"
                f"{'aes' if encrypted else 'clear'}"
            )
            number = port()
            received = artifacts / f"{label}.ts"
            gst = gst_command(
                robot,
                sending,
                uri(number, "caller" if sending else "listener", encrypted),
                fixture if sending else received,
                0 if sending else remuxed.stat().st_size,
            )
            ff_url = uri(
                number, "listener" if sending else "caller", encrypted, ffmpeg=True
            )
            ff_cmd = [
                str(ffmpeg),
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "warning",
                "-y",
            ]
            if not sending:
                ff_cmd += ["-re", "-stream_loop", "-1"]
            ff_cmd += [
                "-i",
                ff_url if sending else str(fixture),
                "-map",
                "0:v:0",
                "-c",
                "copy",
                "-f",
                "mpegts",
                str(received) if sending else ff_url,
            ]
            ff = (ff_cmd, None)
            pair(
                gst if sending else ff,
                ff if sending else gst,
                artifacts,
                label,
                number,
                ffmpeg_sender=not sending,
                ffmpeg_receiver=sending,
            )
            if extract(ffmpeg, received, artifacts / f"{label}.m2v") != expected_video:
                raise RuntimeError(f"{label}: elementary stream differs")
            print(f"PASS {label}", flush=True)

    for mode in ("stop-source", "stop-sink"):
        peer, env = robot
        output = run(
            [str(peer), mode, uri(port(), "listener"), str(fixture), "0"], env=env
        )
        (artifacts / f"{mode}.log").write_text(output)
        if "STOP cycles=5" not in output:
            raise RuntimeError(f"{mode} did not complete")
        print(f"PASS {mode} (5 cycles)", flush=True)

    # Keep one listener alive across two independent connections,
    # with admission rejection in between.
    number = port()
    received = artifacts / "reconnect.ts"
    receiver = gst_command(
        robot,
        False,
        uri(number, "listener", True),
        received,
        len(payload) * 2,
        repeat=True,
    )
    listener_log = artifacts / "reconnect-listener.log"
    with process(*receiver, listener_log) as listener:
        wait_listener(listener, number)
        attempts = (
            ("robotweax-test", False),
            ("rejected-stream", False),
            ("robotweax-test", True),
            ("robotweax-test", False),
        )
        for index, (streamid, wrong_key) in enumerate(attempts):
            allowed = streamid == "robotweax-test" and not wrong_key
            url = uri(number, "caller", True, streamid=streamid)
            if wrong_key:
                url = url.replace("robotweax-gstreamer-test", "deliberately-wrong-key")
            sender = gst_command(robot, True, url, fixture)
            with process(
                *sender, artifacts / f"reconnect-caller-{index}.log"
            ) as caller:
                if allowed:
                    expected_size = len(payload) if index == 0 else len(payload) * 2
                    deadline = time.monotonic() + 15
                    while (
                        not received.exists() or received.stat().st_size < expected_size
                    ):
                        if caller.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError(
                                "reconnect receiver did not drain its input"
                            )
                        time.sleep(0.02)
                    release_sender(caller)
                status = caller.wait(timeout=25)
                if (status == 0) != allowed:
                    raise RuntimeError(
                        "stream-ID admission returned unexpected caller status"
                    )
        require_success(listener, "reconnect listener")
    if (
        received.read_bytes() != payload * 2
        or "CALLER rejected rejected-stream" not in listener_log.read_text()
    ):
        raise RuntimeError("reconnect or stream-ID rejection evidence missing")
    print(
        "PASS encrypted keep-listening reconnect, stream-ID and wrong-key rejection",
        flush=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gst-prefix", type=Path, required=True)
    parser.add_argument("--srt-prefix", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--reference-gst-prefix", type=Path)
    parser.add_argument("--reference-srt-prefix", type=Path)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    if bool(args.reference_gst_prefix) != bool(args.reference_srt_prefix):
        parser.error("both reference prefixes must be supplied together")
    artifacts = (
        args.artifacts.resolve()
        if args.artifacts
        else Path(tempfile.mkdtemp(prefix="robotweax-gstreamer-smoke-"))
    )
    artifacts.mkdir(parents=True, exist_ok=True)
    print(f"Artifacts: {artifacts}", flush=True)
    try:
        qualify(args, artifacts)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout, file=sys.stderr)
        for log in sorted(artifacts.glob("*.log")):
            print(f"{log.name}:\n{log.read_text()[-6000:]}", file=sys.stderr)
        return 1
    print("GStreamer SRT qualification passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
