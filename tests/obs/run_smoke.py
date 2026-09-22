#!/usr/bin/env python3
"""Exercise production OBS media modules through libobs, not an SRT substitute."""
from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import time
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit

spec = importlib.util.spec_from_file_location(
    "gst_smoke", Path(__file__).resolve().parents[1] / "gstreamer/run_smoke.py"
)
gst = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gst)
SECRET = "robotweax-gstreamer-test"  # Public synthetic fixture, not a credential.
MEDIA = re.compile(
    r"^MEDIA video=(\d+) changed=(\d+) audio=(\d+) audible=(\d+) bytes=(\d+)$", re.M
)


def progress(log):
    samples = MEDIA.findall(log.read_text())
    return tuple(map(int, samples[-1])) if samples else (0, 0, 0, 0, 0)


def require_media(log):
    video, changed, audio, audible, _ = progress(log)
    if video < 20 or changed < 10 or audio < 20 or audible < 10:
        raise RuntimeError(
            f"{log.name}: insufficient decoded moving video/non-silent audio"
        )


def require_provider(log, obs_prefix, srt_prefix, ffmpeg_prefix):
    paths = set(re.findall(r"^MAP .*? (/[^\n]+)$", log.read_text(), re.M))
    expected = {
        "librobotweax-srt.so": srt_prefix / "lib",
        "libavformat.so": ffmpeg_prefix / "lib",
        "obs-ffmpeg.so": obs_prefix / "lib/obs-plugins",
    }
    for name, directory in expected.items():
        matches = {
            Path(path).resolve() for path in paths if Path(path).name.startswith(name)
        }
        if len(matches) != 1 or next(iter(matches)).parent != directory.resolve():
            raise RuntimeError(f"{log.name}: missing or unexpected provider for {name}")
    if any(Path(path).name.startswith("libsrt") for path in paths):
        raise RuntimeError(f"{log.name}: a second SRT provider is loaded")
    bindings = re.findall(r"^BINDING (native|ffmpeg) (.+)$", log.read_text(), re.M)
    expected_library = (srt_prefix / "lib/librobotweax-srt.so").resolve()
    if {name for name, _ in bindings} != {"native", "ffmpeg"} or any(
        Path(path).resolve() != expected_library for _, path in bindings
    ):
        raise RuntimeError(f"{log.name}: native/FFmpeg SRT symbol binding differs")


def wait_for(child, log, predicate, description, timeout=25):
    deadline = time.monotonic() + timeout
    while not predicate():
        if child.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(f"{log.name}: {description}; process={child.poll()}")
        time.sleep(0.05)


def command(child, text):
    child.stdin.write((text + "\n").encode())
    child.stdin.flush()


def shutdown(child, log):
    command(child, "quit")
    if child.wait(timeout=8) != 0 or "SHUTDOWN" not in log.read_text():
        raise RuntimeError(f"{log.name}: unclean OBS shutdown")


def obs_provider(args, artifacts):
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("LD_", "DYLD_", "ROBOTWEAX_OBS_"))
    }
    env.update(
        PKG_CONFIG_PATH=str(args.obs_prefix / "lib/pkgconfig"),
        LIBGL_ALWAYS_SOFTWARE="1",
        LC_ALL="C",
        ROBOTWEAX_OBS_STREAMID="robotweax-test",
        ROBOTWEAX_OBS_AVFORMAT=str(args.ffmpeg_prefix / "lib/libavformat.so"),
    )
    flags = shlex.split(
        gst.run(["pkg-config", "--cflags", "--libs", "libobs"], env=env)
    )
    if gst.run(
        ["pkg-config", "--variable=pcfiledir", "libobs"], env=env
    ).strip() != str(args.obs_prefix / "lib/pkgconfig"):
        raise RuntimeError("unexpected libobs pkg-config provider")
    peer = artifacts / "obs-peer"
    gst.run(
        [
            *shlex.split(os.environ.get("CC", "cc")),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(Path(__file__).with_name("peer.c")),
            "-o",
            str(peer),
            *flags,
            f"-Wl,-rpath,{args.obs_prefix / 'lib'}",
            "-ldl",
        ],
        env=env,
    )
    return peer, env


def obs_command(
    provider,
    prefix,
    source,
    destination="-",
    *,
    local=False,
    encrypted=False,
    wrong_key=False,
    streamid=None,
):
    peer, base = provider
    env = dict(base)
    if encrypted:
        env["ROBOTWEAX_OBS_PASSPHRASE"] = (
            "deliberately-wrong-key" if wrong_key else SECRET
        )
    if streamid is not None:
        env["ROBOTWEAX_OBS_STREAMID"] = streamid
    # Exercise OBS service fields; URL options otherwise override them and
    # could accidentally turn a wrong-key test into a successful connection.
    if destination != "-":
        parts = urlsplit(str(destination))
        query = [
            (key, value)
            for key, value in parse_qsl(parts.query)
            if key not in {"streamid", "passphrase"}
        ]
        destination = urlunsplit(parts._replace(query=urlencode(query)))
    return [
        str(peer),
        str(prefix),
        str(source),
        str(destination),
        "local" if local else "network",
    ], env


def decoded_file(ffmpeg, path, artifacts, label):
    frames = artifacts / f"{label}.frames"
    frames.write_text(
        gst.run(
            [
                str(ffmpeg),
                "-v",
                "error",
                "-i",
                str(path),
                "-map",
                "0:v:0",
                "-f",
                "framehash",
                "-",
            ]
        )
    )
    hashes = re.findall(r"^0,.*?, ([0-9a-f]{64})$", frames.read_text(), re.M)
    if len(hashes) < 20 or len(set(hashes)) < 10:
        raise RuntimeError(f"{label}: output does not decode to moving video")
    audio = subprocess.run(
        [
            str(ffmpeg),
            "-v",
            "error",
            "-i",
            str(path),
            "-map",
            "0:a:0",
            "-f",
            "s16le",
            "-",
        ],
        check=True,
        capture_output=True,
        timeout=30,
    ).stdout
    if len(audio) < 48000 or sum(byte != 0 for byte in audio) < 1000:
        raise RuntimeError(f"{label}: output has no decoded non-silent audio")


def qualify(args, artifacts):
    obs = obs_provider(args, artifacts)
    reference = gst.provider(
        args.gst_prefix, args.reference_srt_prefix, artifacts, "haivision"
    )
    ffmpeg = args.ffmpeg_prefix / "bin/ffmpeg"
    fixture = artifacts / "fixture.ts"
    gst.run(
        [
            str(ffmpeg),
            "-v",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=320x180:rate=25",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=997:sample_rate=48000",
            "-t",
            "8",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-tune",
            "zerolatency",
            "-g",
            "25",
            "-b:v",
            "500k",
            "-c:a",
            "aac",
            "-b:a",
            "128k",
            "-f",
            "mpegts",
            str(fixture),
        ]
    )
    decoded_file(ffmpeg, fixture, artifacts, "fixture")

    def verify(log):
        require_provider(log, args.obs_prefix, args.srt_prefix, args.ffmpeg_prefix)

    for sending in (False, True):
        for encrypted in (False, True):
            for obs_listener in (False, True):
                label = (
                    f"{'send' if sending else 'receive'}-"
                    f"{'aes' if encrypted else 'clear'}-"
                    f"{'listener' if obs_listener else 'caller'}"
                )
                number = gst.port()
                obs_url = gst.uri(
                    number,
                    "listener" if obs_listener else "caller",
                    encrypted,
                    ffmpeg=True,
                )
                ref_url = gst.uri(
                    number, "caller" if obs_listener else "listener", encrypted
                )
                output = artifacts / f"{label}.ts"
                obs_cmd = obs_command(
                    obs,
                    args.obs_prefix,
                    fixture if sending else obs_url,
                    obs_url if sending else "-",
                    local=sending,
                    encrypted=encrypted,
                )
                ref_cmd = gst.gst_command(
                    reference,
                    not sending,
                    ref_url,
                    output if sending else fixture,
                    200000 if sending else 0,
                )
                obs_log, ref_log = (
                    artifacts / f"{label}-obs.log",
                    artifacts / f"{label}-reference.log",
                )
                first, second = (
                    (obs_cmd, ref_cmd) if obs_listener else (ref_cmd, obs_cmd)
                )
                first_log, second_log = (
                    (obs_log, ref_log) if obs_listener else (ref_log, obs_log)
                )
                with gst.process(*first, first_log) as listener:
                    gst.wait_listener(listener, number)
                    with gst.process(*second, second_log) as caller:
                        ob, ref = (
                            (listener, caller) if obs_listener else (caller, listener)
                        )
                        if sending:
                            gst.require_success(ref, label)
                        else:
                            wait_for(
                                ob,
                                obs_log,
                                lambda: min(progress(obs_log)[:4]) >= 25,
                                "decoded A/V",
                            )
                            gst.release_sender(ref)
                            gst.require_success(ref, label)
                        shutdown(ob, obs_log)
                verify(obs_log)
                require_media(obs_log)
                if sending:
                    decoded_file(ffmpeg, output, artifacts, label)
                print(f"PASS {label}", flush=True)

    # Exercise both paths in ONE OBS process; independent reference peers at
    # either end prevent an accidental Robotweax-to-Robotweax-only assertion.
    incoming, outgoing = gst.port(), gst.port()
    output = artifacts / "duplex.ts"
    source = gst.gst_command(
        reference, True, gst.uri(incoming, "caller", True), fixture
    )
    sink = gst.gst_command(
        reference, False, gst.uri(outgoing, "listener", True), output, 200000
    )
    relay = obs_command(
        obs,
        args.obs_prefix,
        gst.uri(incoming, "listener", True, ffmpeg=True),
        gst.uri(outgoing, "caller", True, ffmpeg=True),
        encrypted=True,
    )
    log = artifacts / "duplex-obs.log"
    with gst.process(*sink, artifacts / "duplex-sink.log") as rx:
        gst.wait_listener(rx, outgoing)
        with gst.process(*relay, log) as ob:
            gst.wait_listener(ob, incoming)
            with gst.process(*source, artifacts / "duplex-source.log") as tx:
                gst.require_success(rx, "duplex receiver")
                gst.release_sender(tx)
                gst.require_success(tx, "duplex sender")
                shutdown(ob, log)
    verify(log)
    require_media(log)
    decoded_file(ffmpeg, output, artifacts, "duplex")
    print("PASS simultaneous encrypted receive and native send", flush=True)

    for reason in ("wrong-key", "streamid"):
        number = gst.port()
        target = artifacts / f"reject-{reason}.ts"
        sink = gst.gst_command(
            reference,
            False,
            gst.uri(number, "listener", True),
            target,
            200000,
            repeat=True,
        )
        sink = (sink[0], {**sink[1], "GST_DEBUG": "srt*:6", "GST_DEBUG_NO_COLOR": "1"})
        ref_log = artifacts / f"reject-{reason}-reference.log"
        with gst.process(*sink, ref_log) as rx:
            gst.wait_listener(rx, number)
            bad = obs_command(
                obs,
                args.obs_prefix,
                fixture,
                gst.uri(number, "caller", True, ffmpeg=True),
                local=True,
                encrypted=True,
                wrong_key=reason == "wrong-key",
                streamid="denied" if reason == "streamid" else None,
            )
            log = artifacts / f"reject-{reason}-obs.log"
            with gst.process(*bad, log) as ob:
                expected = (
                    "BADSECRET" if reason == "wrong-key" else "CALLER rejected denied"
                )
                wait_for(
                    rx,
                    ref_log,
                    lambda: expected in ref_log.read_text(),
                    "explicit rejection",
                )
                if target.exists() and target.stat().st_size:
                    raise RuntimeError("rejected caller delivered media")
                shutdown(ob, log)
            verify(log)
            good = obs_command(
                obs,
                args.obs_prefix,
                fixture,
                gst.uri(number, "caller", True, ffmpeg=True),
                local=True,
                encrypted=True,
            )
            log = artifacts / f"recovery-{reason}-obs.log"
            with gst.process(*good, log) as ob:
                gst.require_success(rx, "recovery")
                shutdown(ob, log)
            verify(log)
        decoded_file(ffmpeg, target, artifacts, f"recovery-{reason}")
        print(f"PASS {reason} rejection and valid subsequent connection", flush=True)

    # Reuse one native output and one process across connection loss/restart.
    number = gst.port()
    url = gst.uri(number, "caller", ffmpeg=True)
    log = artifacts / "restart-obs.log"
    sender = obs_command(obs, args.obs_prefix, fixture, url, local=True)
    with gst.process(*sender, log) as ob:
        for cycle in range(3):
            target = artifacts / f"restart-{cycle}.ts"
            sink = gst.gst_command(
                reference, False, gst.uri(number, "listener"), target, 200000
            )
            with gst.process(*sink, artifacts / f"restart-{cycle}-reference.log") as rx:
                gst.wait_listener(rx, number)
                if cycle:
                    command(ob, "restart")
                gst.require_success(rx, "restart receiver")
            decoded_file(ffmpeg, target, artifacts, f"restart-{cycle}")
        shutdown(ob, log)
    verify(log)
    if log.read_text().count("RESTARTED\n") != 2:
        raise RuntimeError("native output did not complete both explicit restarts")
    print("PASS native output connection loss and three start/stop cycles", flush=True)

    # Interrupt waiting inputs and outputs; successful cleanup is mandatory.
    for sending in (False, True):
        number = gst.port()
        url = gst.uri(number, "listener", ffmpeg=True)
        log = artifacts / f"waiting-{'output' if sending else 'input'}.log"
        cmd = obs_command(
            obs,
            args.obs_prefix,
            fixture if sending else url,
            url if sending else "-",
            local=sending,
        )
        with gst.process(*cmd, log) as ob:
            wait_for(ob, log, lambda: "READY" in log.read_text(), "startup")
            gst.wait_listener(ob, number)
            shutdown(ob, log)
        verify(log)
    print("PASS shutdown while input/output await a connection", flush=True)


def main():
    parser = argparse.ArgumentParser()
    for name in (
        "obs-prefix",
        "srt-prefix",
        "ffmpeg-prefix",
        "gst-prefix",
        "reference-srt-prefix",
        "artifacts",
    ):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    for name, value in vars(args).items():
        setattr(args, name, value.resolve())
    args.artifacts.mkdir(parents=True, exist_ok=True)
    qualify(args, args.artifacts)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stdout, flush=True)
        print(error.stderr, flush=True)
        raise
