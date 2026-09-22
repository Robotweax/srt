#!/usr/bin/env python3
"""Qualify installed VLC SRT modules, decoded media, and controlled shutdown."""

from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time


GST_DRIVER = Path(__file__).resolve().parents[1] / "gstreamer/run_smoke.py"
spec = importlib.util.spec_from_file_location("gst_smoke", GST_DRIVER)
gst = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gst)
SECRET = "robotweax-gstreamer-test"  # Public synthetic fixture only.


def hashes(log):
    frames = log.with_suffix(".frames")
    return re.findall(
        r"^FRAME ([0-9a-f]{16})$",
        frames.read_text() if frames.exists() else "",
        re.MULTILINE,
    )


def process(command, env, log):
    if env is not None and "ROBOTWEAX_VLC_PLUGIN" in env:
        env = {**env, "ROBOTWEAX_VLC_FRAME_LOG": str(log.with_suffix(".frames"))}
    return gst.process(command, env, log)


def vlc_provider(prefix, srt_prefix, artifacts):
    prefix, srt_prefix = prefix.resolve(), srt_prefix.resolve()
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("VLC_", "LD_", "DYLD_", "ROBOTWEAX_VLC_"))
    }
    env.update(
        {
            "VLC_PLUGIN_PATH": str(prefix / "lib/vlc/plugins"),
            "PKG_CONFIG_PATH": str(prefix / "lib/pkgconfig"),
            "ROBOTWEAX_SRT_LIBRARY_DIR": str((srt_prefix / "lib").resolve()),
            "LC_ALL": "C",
        }
    )
    peer = artifacts / "vlc-peer"
    pc_directory = prefix / "lib/pkgconfig"
    if gst.run(
        ["pkg-config", "--variable=pcfiledir", "libvlc"], env=env
    ).strip() != str(pc_directory):
        raise RuntimeError("pkg-config selected another libVLC installation")
    flags = shlex.split(
        gst.run(["pkg-config", "--cflags", "--libs", "libvlc"], env=env)
    )
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
            "-ldl",
            f"-Wl,-rpath,{prefix / 'lib'}",
        ],
        env=env,
    )
    return peer, env, prefix


def vlc_command(provider, mode, source, argument, encrypted=False, wrong_key=False):
    peer, base_env, prefix = provider
    env = dict(base_env)
    module = (
        "access_output/libaccess_output_srt_plugin.so"
        if mode == "send"
        else "access/libaccess_srt_plugin.so"
    )
    env["ROBOTWEAX_VLC_PLUGIN"] = str(prefix / "lib/vlc/plugins" / module)
    if encrypted:
        env["ROBOTWEAX_VLC_TEST_PASSPHRASE"] = (
            "deliberately-wrong-key" if wrong_key else SECRET
        )
    return [str(peer), mode, str(source), str(argument)], env


def require_frames(log, expected, minimum=15):
    actual = hashes(log)
    if len(actual) < minimum or len(set(actual)) < 5:
        raise RuntimeError(f"{log.name}: insufficient decoded frame diversity")
    if not set(actual).issubset(expected):
        raise RuntimeError(f"{log.name}: decoded video differs from local reference")
    if 'using access module "access_srt"' not in log.read_text():
        raise RuntimeError(f"{log.name}: missing actual VLC SRT input evidence")


def wait_text(child, log, text, timeout=8):
    deadline = time.monotonic() + timeout
    while text not in log.read_text():
        if child.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(f"{log.name}: missing evidence: {text}")
        time.sleep(0.02)


def rejection_and_recovery(
    vlc, reference, fixture, ffmpeg, original, artifacts, wrong_key
):
    label = "wrong-key" if wrong_key else "streamid"
    number = gst.port()
    target = artifacts / f"rejection-{label}.ts"
    log = artifacts / f"rejection-{label}-listener.log"
    receiver = gst.gst_command(
        reference, False, gst.uri(number, "listener", True), target, 65536, repeat=True
    )
    receiver = (
        receiver[0],
        {**receiver[1], "GST_DEBUG": "srt*:6", "GST_DEBUG_NO_COLOR": "1"},
    )
    bad_command, bad_env = vlc_command(
        vlc, "send", fixture, f"127.0.0.1:{number}", True, wrong_key
    )
    if not wrong_key:
        bad_env["ROBOTWEAX_VLC_TEST_STREAMID"] = "denied"
    with process(*receiver, log) as rx:
        gst.wait_listener(rx, number)
        with process(
            bad_command, bad_env, artifacts / f"rejection-{label}-caller.log"
        ) as tx:
            wait_text(rx, log, "BADSECRET" if wrong_key else "CALLER rejected denied")
            if target.exists() and target.stat().st_size != 0:
                raise RuntimeError("rejected caller delivered media")
            gst.release_sender(tx)
            gst.require_success(tx, "rejected caller shutdown")
        sender = vlc_command(vlc, "send", fixture, f"127.0.0.1:{number}", True)
        with process(*sender, artifacts / f"rejection-{label}-recovery.log") as tx:
            gst.require_success(rx, "valid caller after rejection")
            gst.release_sender(tx)
            gst.require_success(tx, "valid caller shutdown")
    elementary = gst.extract(ffmpeg, target, artifacts / f"rejection-{label}.m2v")
    if len(elementary) < 10000 or not original.startswith(elementary):
        raise RuntimeError("recovery media differs")
    print(f"PASS {label} rejection and valid subsequent connection", flush=True)


def decode_pair(
    vlc, source, artifacts, label, number, expected, encrypted=False, vlc_listener=True
):
    url = f"srt://127.0.0.1:{number}?mode={'listener' if vlc_listener else 'caller'}"
    receiver = vlc_command(vlc, "decode", url, 20, encrypted)
    first, second = (receiver, source) if vlc_listener else (source, receiver)
    first_log = artifacts / f"{label}-listener.log"
    second_log = artifacts / f"{label}-caller.log"
    with process(*first, first_log) as listener:
        gst.wait_listener(listener, number)
        with process(*second, second_log) as caller:
            rx, tx = (listener, caller) if vlc_listener else (caller, listener)
            gst.require_success(rx, label)
            # The finite GStreamer peer awaits receiver-confirmed completion.
            if Path(source[0][0]).name.startswith("peer-"):
                gst.release_sender(tx)
                gst.require_success(tx, label + " source")
    require_frames(first_log if vlc_listener else second_log, expected)
    print(f"PASS {label}", flush=True)


def qualify(args, artifacts):
    vlc = vlc_provider(args.vlc_prefix, args.srt_prefix, artifacts)
    reference = gst.provider(
        args.gst_prefix, args.reference_srt_prefix, artifacts, "haivision"
    )
    ffmpeg = args.ffmpeg.resolve()
    fixture = artifacts / "fixture.ts"
    gst.run(
        [
            str(ffmpeg),
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=128x96:rate=25",
            "-t",
            "2",
            "-an",
            "-c:v",
            "mpeg2video",
            "-g",
            "1",
            "-bf",
            "0",
            "-f",
            "mpegts",
            str(fixture),
        ]
    )
    baseline_log = artifacts / "baseline.log"
    with process(*vlc_command(vlc, "decode", fixture, 0), baseline_log) as peer:
        gst.require_success(peer, "local decode")
    expected = set(hashes(baseline_log))
    if len(expected) < 10:
        raise RuntimeError("local VLC reference did not decode diverse video")
    print("PASS local MPEG-2 decoding reference", flush=True)

    for encrypted in (False, True):
        cipher = "aes" if encrypted else "clear"
        number = gst.port()
        ff_url = gst.uri(number, "caller", encrypted, ffmpeg=True)
        source = (
            [
                str(ffmpeg),
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "error",
                "-re",
                "-stream_loop",
                "-1",
                "-i",
                str(fixture),
                "-c",
                "copy",
                "-f",
                "mpegts",
                ff_url,
            ],
            None,
        )
        decode_pair(
            vlc,
            source,
            artifacts,
            f"ffmpeg-to-vlc-{cipher}",
            number,
            expected,
            encrypted,
        )
        for vlc_listener in (True, False):
            number = gst.port()
            source = gst.gst_command(
                reference,
                True,
                gst.uri(number, "caller" if vlc_listener else "listener", encrypted),
                fixture,
            )
            decode_pair(
                vlc,
                source,
                artifacts,
                f"haivision-gst-to-vlc-{cipher}-"
                f"{'listener' if vlc_listener else 'caller'}",
                number,
                expected,
                encrypted,
                vlc_listener,
            )

    # VLC's output is a live caller. Compare an elementary-media prefix; do
    # not infer finite-transfer EOF guarantees from a bounded live capture.
    original = gst.extract(ffmpeg, fixture, artifacts / "expected.m2v")
    for encrypted in (False, True):
        cipher = "aes" if encrypted else "clear"
        number = gst.port()
        target = artifacts / f"vlc-to-haivision-{cipher}.ts"
        receiver = gst.gst_command(
            reference, False, gst.uri(number, "listener", encrypted), target, 65536
        )
        sender = vlc_command(vlc, "send", fixture, f"127.0.0.1:{number}", encrypted)
        with process(
            *receiver, artifacts / f"vlc-to-haivision-{cipher}-receiver.log"
        ) as rx:
            gst.wait_listener(rx, number)
            with process(
                *sender, artifacts / f"vlc-to-haivision-{cipher}-sender.log"
            ) as tx:
                gst.require_success(rx, "VLC live output receiver")
                gst.release_sender(tx)
                gst.require_success(tx, "VLC live output sender")
        elementary = gst.extract(
            ffmpeg, target, artifacts / f"vlc-to-haivision-{cipher}.m2v"
        )
        if len(elementary) < 10000 or not original.startswith(elementary):
            raise RuntimeError("VLC output elementary payload differs")
        print(f"PASS vlc-to-haivision-{cipher}", flush=True)

        number = gst.port()
        target = artifacts / f"vlc-to-ffmpeg-{cipher}.ts"
        receiver = (
            [
                str(ffmpeg),
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-i",
                gst.uri(number, "listener", encrypted, ffmpeg=True),
                "-t",
                "0.6",
                "-map",
                "0:v:0",
                "-c",
                "copy",
                "-f",
                "mpegts",
                str(target),
            ],
            None,
        )
        sender = vlc_command(vlc, "send", fixture, f"127.0.0.1:{number}", encrypted)
        with process(
            *receiver, artifacts / f"vlc-to-ffmpeg-{cipher}-receiver.log"
        ) as rx:
            gst.wait_listener(rx, number)
            with process(
                *sender, artifacts / f"vlc-to-ffmpeg-{cipher}-sender.log"
            ) as tx:
                gst.require_success(rx, "FFmpeg live capture")
                gst.release_sender(tx)
                gst.require_success(tx, "VLC sender shutdown")
        elementary = gst.extract(
            ffmpeg, target, artifacts / f"vlc-to-ffmpeg-{cipher}.m2v"
        )
        if len(elementary) < 10000 or not original.startswith(elementary):
            raise RuntimeError("VLC-to-FFmpeg elementary payload differs")
        print(f"PASS vlc-to-ffmpeg-{cipher}", flush=True)

    for wrong_key in (False, True):
        rejection_and_recovery(
            vlc, reference, fixture, ffmpeg, original, artifacts, wrong_key
        )

    second_fixture = artifacts / "second-fixture.ts"
    gst.run(
        [
            str(ffmpeg),
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=128x96:rate=25",
            "-ss",
            "5",
            "-t",
            "2",
            "-an",
            "-c:v",
            "mpeg2video",
            "-g",
            "1",
            "-bf",
            "0",
            "-f",
            "mpegts",
            str(second_fixture),
        ]
    )
    second_baseline = artifacts / "second-baseline.log"
    with process(
        *vlc_command(vlc, "decode", second_fixture, 0), second_baseline
    ) as peer:
        gst.require_success(peer, "second local decode")
    second_expected = set(hashes(second_baseline)) - expected
    if len(second_expected) < 15:
        raise RuntimeError("reconnect fixture is not distinguishable")
    number = gst.port()
    log = artifacts / "reconnect-listener.log"
    receiver = vlc_command(
        vlc, "observe", f"srt://127.0.0.1:{number}?mode=listener", 0, True
    )
    with process(*receiver, log) as rx:
        gst.wait_listener(rx, number)
        sender = gst.gst_command(
            reference, True, gst.uri(number, "caller", True), fixture
        )
        with process(*sender, artifacts / "reconnect-first-caller.log") as tx:
            deadline = time.monotonic() + 15
            while len(hashes(log)) < 20:
                if rx.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("first connection did not decode")
                time.sleep(0.02)
            if not set(hashes(log)[:20]).issubset(expected):
                raise RuntimeError("initial reconnect media differs")
            wait_text(tx, artifacts / "reconnect-first-caller.log", "DONE bytes=")
            gst.release_sender(tx)
            gst.require_success(tx, "first caller disconnect")
        time.sleep(1)  # Bounded loopback recovery opportunity, not a network SLA.
        sender = gst.gst_command(
            reference, True, gst.uri(number, "caller", True), second_fixture
        )
        with process(*sender, artifacts / "reconnect-second-caller.log") as tx:
            deadline = time.monotonic() + 15
            while len(set(hashes(log)) & second_expected) < 15:
                if rx.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(
                        "VLC did not decode distinct media after reconnect"
                    )
                time.sleep(0.02)
            wait_text(tx, artifacts / "reconnect-second-caller.log", "DONE bytes=")
            gst.release_sender(tx)
            gst.require_success(tx, "second caller shutdown")
            gst.release_sender(rx)
            gst.require_success(rx, "VLC listener reconnect")
    print("PASS encrypted VLC listener reconnect (media before and after)", flush=True)

    number = gst.port()
    command = vlc_command(vlc, "stop", f"srt://127.0.0.1:{number}?mode=listener", 5)
    log = artifacts / "blocked-stop.log"
    with process(*command, log) as peer:
        for cycle in range(5):
            deadline = time.monotonic() + 5
            while f"STARTED cycle={cycle}" not in log.read_text():
                if peer.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("VLC stop cycle did not start")
                time.sleep(0.02)
            gst.wait_listener(peer, number)
            time.sleep(0.2)  # Enter the adapter's unbounded epoll wait.
            gst.release_sender(peer)
        gst.require_success(peer, "blocked stop")
    if len(re.findall(r"^STOP cycle=", log.read_text(), re.MULTILINE)) != 5:
        raise RuntimeError("missing stop-cycle evidence")
    print("PASS blocked listener stop (5 cycles)", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in (
        "vlc-prefix",
        "srt-prefix",
        "gst-prefix",
        "reference-srt-prefix",
        "ffmpeg",
    ):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    artifacts = (
        args.artifacts.resolve()
        if args.artifacts
        else Path(tempfile.mkdtemp(prefix="robotweax-vlc-"))
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
            print(f"{log.name}:\n{log.read_text()[-4000:]}", file=sys.stderr)
        return 1
    print("VLC SRT qualification passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
