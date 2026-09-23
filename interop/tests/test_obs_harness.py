from __future__ import annotations

from array import array
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tests/obs" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


obs = load("obs_smoke", "run_smoke.py")
prepare = load("obs_prepare", "prepare_source.py")
desktop = load("obs_desktop", "run_desktop_smoke.py")
lifecycle = load("obs_desktop_lifecycle", "prepare_desktop_lifecycle.py")
windows_prepare = load("obs_windows_prepare", "prepare_windows_source.py")
windows = load("obs_windows_smoke", "run_windows_smoke.py")
windows_desktop = load("obs_windows_desktop", "run_windows_desktop_smoke.py")
windows_preview = load("obs_windows_preview", "package_windows_preview.py")


class ObsHarnessTests(unittest.TestCase):
    def test_windows_preview_isolated_deterministic_and_provider_guarded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "obs"
            source = root / "obs-source"
            robotweax = root / "robotweax"
            dependency = root / "dependency"
            qt = root / "qt"
            for relative in windows_preview.REQUIRED_RUNTIME:
                target = prefix / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"robotweax" if relative.endswith("/srt.dll") else relative.encode())
            for relative in ("bin/64bit/windows-obs-peer.exe", "bin/64bit/obs.pdb",
                             "config/obs-studio/user.ini"):
                target = prefix / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("not part of the preview")
            for relative in ("deps", "libobs", "shared", "frontend",
                             "plugins/obs-ffmpeg", "plugins/obs-x264",
                             "plugins/rtmp-services", "plugins/obs-transitions"):
                (source / relative).mkdir(parents=True)
            (source / "COPYING").write_text("OBS GPL test fixture")
            (robotweax / "LICENSE").parent.mkdir()
            (robotweax / "LICENSE").write_text("Robotweax MIT test fixture")
            (dependency / "licenses/FFmpeg").mkdir(parents=True)
            (dependency / "licenses/FFmpeg/COPYING.GPLv2").write_text("FFmpeg fixture")
            (qt / "licenses/qt6").mkdir(parents=True)
            (qt / "licenses/qt6/LICENSE.GPL2").write_text("Qt fixture")
            selected = root / "selected-srt.dll"
            selected.write_bytes(b"robotweax")
            reference = root / "reference-srt.dll"
            reference.write_bytes(b"reference")
            args = SimpleNamespace(
                obs_prefix=prefix, obs_source=source,
                robotweax_source=robotweax, robotweax_dll=selected,
                reference_srt=reference, dependency_prefix=dependency,
                qt_prefix=qt, output_dir=root / "first",
            )
            with mock.patch.object(windows_preview, "git_head", side_effect=[
                windows_preview.OBS_COMMIT, "robotweax-test-commit",
            ]):
                first = windows_preview.build(args)
            manifest = json.loads((args.output_dir / "MANIFEST.json").read_text())
            windows_preview.verify_archive(first, manifest)
            names = {item["path"] for item in manifest["files"]}
            self.assertIn("bin/64bit/obs64.exe", names)
            self.assertIn("bin/64bit/srt.dll", names)
            self.assertIn("LICENSES/obs-deps/FFmpeg/COPYING.GPLv2", names)
            self.assertIn("LICENSES/obs-qt/qt6/LICENSE.GPL2", names)
            self.assertNotIn("bin/64bit/windows-obs-peer.exe", names)
            self.assertNotIn("bin/64bit/obs.pdb", names)
            self.assertFalse(any(name.startswith("config/") for name in names))
            with self.assertRaisesRegex(RuntimeError, "fresh preview output"):
                windows_preview.build(args)
            args.output_dir = root / "second"
            with mock.patch.object(windows_preview, "git_head", side_effect=[
                windows_preview.OBS_COMMIT, "robotweax-test-commit",
            ]):
                second = windows_preview.build(args)
            self.assertEqual(windows_preview.digest(first), windows_preview.digest(second))
            (prefix / "obs-plugins/64bit/srt.dll").write_bytes(b"competing")
            with mock.patch.object(windows_preview, "git_head", side_effect=[
                windows_preview.OBS_COMMIT, "robotweax-test-commit",
            ]):
                with self.assertRaisesRegex(RuntimeError, "unexpected SRT providers"):
                    windows_preview.collect(args)
            (prefix / "obs-plugins/64bit/srt.dll").unlink()
            (prefix / "bin/64bit/srt.dll").write_bytes(b"reference")
            with mock.patch.object(windows_preview, "git_head", side_effect=[
                windows_preview.OBS_COMMIT, "robotweax-test-commit",
            ]):
                with self.assertRaisesRegex(RuntimeError, "selected Robotweax SRT DLL"):
                    windows_preview.collect(args)

    def test_windows_desktop_profile_is_isolated_and_rejects_reuse(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            config = windows_desktop.prepare_profile(
                prefix,
                prefix / "fixture.ts",
                "srt://127.0.0.1:12345?mode=caller",
            )
            self.assertEqual(config, prefix / "config/obs-studio")
            user = (config / "user.ini").read_text()
            self.assertIn("ConfirmOnExit=false", user)
            profile = (config / "basic/profiles/Robotweax/basic.ini").read_text()
            self.assertIn("Reconnect=true", profile)
            self.assertIn("MaxRetries=60", profile)
            service = json.loads(
                (config / "basic/profiles/Robotweax/service.json").read_text()
            )
            self.assertEqual(service["type"], "rtmp_custom")
            with self.assertRaises(FileExistsError):
                windows_desktop.prepare_profile(prefix, prefix / "fixture.ts", "srt://other")

    def test_windows_desktop_requires_audible_audio(self):
        with mock.patch.object(windows_desktop.subprocess, "run") as run:
            run.return_value.stdout = array("f", [0.0] * 4800).tobytes()
            with self.assertRaisesRegex(RuntimeError, "no audible decoded audio"):
                windows_desktop.decoded_audio(Path("ffmpeg.exe"), Path("media.ts"), {})
            run.return_value.stdout = array("f", [0.02] * 4800).tobytes()
            windows_desktop.decoded_audio(Path("ffmpeg.exe"), Path("media.ts"), {})

    def test_windows_desktop_switches_only_test_profile_to_encrypted_source(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            config = windows_desktop.prepare_profile(
                prefix, prefix / "fixture.ts", "srt://output-one"
            )
            windows_desktop.use_network_source(
                config, "srt://encrypted-input", "srt://output-two"
            )
            scene = json.loads((config / "basic/scenes/Robotweax.json").read_text())
            media = next(item for item in scene["sources"] if item["name"] == "Media")
            self.assertFalse(media["settings"]["is_local_file"])
            self.assertEqual(media["settings"]["input"], "srt://encrypted-input")
            self.assertEqual(media["settings"]["input_format"], "mpegts")
            service = json.loads(
                (config / "basic/profiles/Robotweax/service.json").read_text()
            )
            self.assertEqual(service["settings"]["server"], "srt://output-two")

    def test_windows_desktop_shutdown_rejects_extra_allocations(self):
        clean = (
            "Loaded scenes:\nStreaming Start\n==== Shutting down\n"
            "SRT connection closed\nOutput 'simple_stream': stopping\n"
            "Freeing OBS context data\nNumber of memory leaks: 1\n"
        )
        self.assertEqual(windows_desktop.check_shutdown(clean, "output"), 1)
        self.assertEqual(windows_desktop.check_shutdown(clean, "source", 1), 1)
        for invalid in (
            clean.replace("==== Shutting down\n", ""),
            clean.replace("SRT connection closed\n", ""),
            clean.replace("Output 'simple_stream': stopping\n", ""),
            clean.replace("Freeing OBS context data\n", ""),
            clean.replace(
                "==== Shutting down\nSRT connection closed\n"
                "Output 'simple_stream': stopping\n",
                "SRT connection closed\nOutput 'simple_stream': stopping\n"
                "==== Shutting down\n",
            ),
        ):
            with self.subTest(invalid=invalid):
                with self.assertRaisesRegex(RuntimeError, "ordered streaming shutdown"):
                    windows_desktop.check_shutdown(invalid, "output")
        with self.assertRaisesRegex(RuntimeError, "allocation baseline regressed"):
            windows_desktop.check_shutdown(clean.replace("leaks: 1", "leaks: 2"), "output")
        with self.assertRaisesRegex(RuntimeError, "allocation baseline regressed"):
            windows_desktop.check_shutdown(clean.replace("leaks: 1", "leaks: 0"), "source", 1)

    def test_windows_dependency_preparation_is_pinned_and_idempotent(self):
        original = (
            b"function(test)\n"
            + windows_prepare.ORIGINAL
            + windows_prepare.QT_ORIGINAL
            + b"endfunction()\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / windows_prepare.TARGET
            target.parent.mkdir(parents=True)
            target.write_bytes(original)
            arch_original = b"function(test)\n" + windows_prepare.ARCH_ORIGINAL + b"endfunction()\n"
            arch_target = root / windows_prepare.ARCH_TARGET
            arch_target.write_bytes(arch_original)
            prepared = original.replace(
                windows_prepare.ORIGINAL, windows_prepare.PREPARED
            ).replace(
                windows_prepare.QT_ORIGINAL, windows_prepare.QT_PREPARED
            )
            arch_prepared = arch_original.replace(
                windows_prepare.ARCH_ORIGINAL, windows_prepare.ARCH_PREPARED
            )
            with mock.patch.multiple(
                windows_prepare,
                ORIGINAL_SHA256=hashlib.sha256(original).hexdigest(),
                PREPARED_SHA256=hashlib.sha256(prepared).hexdigest(),
                ARCH_ORIGINAL_SHA256=hashlib.sha256(arch_original).hexdigest(),
                ARCH_PREPARED_SHA256=hashlib.sha256(arch_prepared).hexdigest(),
            ):
                target.write_bytes(original.replace(b"\n", b"\r\n"))
                arch_target.write_bytes(arch_original.replace(b"\n", b"\r\n"))
                self.assertTrue(windows_prepare.prepare(root))
                self.assertEqual(target.read_bytes().count(windows_prepare.PREPARED), 1)
                self.assertEqual(target.read_bytes().count(windows_prepare.QT_PREPARED), 1)
                self.assertEqual(arch_target.read_bytes(), arch_prepared)
                self.assertFalse(windows_prepare.prepare(root))
                target.write_bytes(target.read_bytes() + b"unknown\n")
                with self.assertRaisesRegex(RuntimeError, "unrecognized"):
                    windows_prepare.prepare(root)
                self.assertEqual(target.read_bytes(), prepared + b"unknown\n")
                self.assertEqual(arch_target.read_bytes(), arch_prepared)

    def test_windows_transport_capture_requires_coherent_mpeg_ts(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.ts"
            packet = bytes([0x47]) + bytes(187)
            capture.write_bytes(packet * 120)
            packets, ratio = windows.ts_packets(capture)
            self.assertEqual(packets, 120)
            self.assertEqual(ratio, 1.0)
            capture.write_bytes(bytes(188 * 120))
            with self.assertRaisesRegex(RuntimeError, "not coherent MPEG-TS"):
                windows.ts_packets(capture)

    def test_windows_capture_reports_decoded_frame_diversity(self):
        first = "0" * 32
        second = "1" * 32
        report = (
            "#format: frame checksums\n"
            f"0, 0, 0, 1, 86400, {first}\n"
            f"0, 1, 1, 1, 86400, {first}\n"
            f"0, 2, 2, 1, 86400, {second}\n"
        )
        self.assertEqual(windows.decoded_frame_hashes(report), (3, 2))
        with self.assertRaisesRegex(RuntimeError, "no decoded video"):
            windows.decoded_frame_hashes("# empty\n")

    def test_windows_capture_requires_moving_decoded_video(self):
        def report(unique):
            return "".join(
                f"0, {index}, {index}, 1, 86400, {index % unique:032x}\n"
                for index in range(20)
            )

        ffmpeg = Path("ffmpeg.exe")
        capture = Path("capture.ts")
        with mock.patch.object(windows.subprocess, "run") as run:
            run.return_value.stdout = report(6)
            with self.assertRaisesRegex(RuntimeError, "insufficient decoded moving"):
                windows.inspect_captured_video(ffmpeg, capture, {})
            run.return_value.stdout = report(11)
            windows.inspect_captured_video(ffmpeg, capture, {})

    def test_windows_synthetic_i420_frames_have_conversion_metadata(self):
        peer = (ROOT / "tests/obs/windows_obs_peer.c").read_text()
        self.assertIn("frame->full_range = false;", peer)
        self.assertIn(
            "VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_I420, frame->color_matrix,",
            peer,
        )
        self.assertIn("frame->color_range_min, frame->color_range_max", peer)
        smoke = (ROOT / "tests/obs/run_windows_smoke.py").read_text()
        self.assertIn("if frames < 20 or unique < 10:", smoke)

    def test_windows_obs_peer_runs_beside_installed_libobs_data(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            runtime = prefix / "bin/64bit"
            runtime.mkdir(parents=True)
            with self.assertRaisesRegex(RuntimeError, "libobs effects"):
                windows.obs_working_directory(prefix)
            effects = prefix / "data/libobs"
            effects.mkdir(parents=True)
            (effects / "default.effect").write_text("synthetic effect")
            self.assertEqual(windows.obs_working_directory(prefix), runtime)
            windows.require_obs_peer_directory(runtime / "windows-obs-peer.exe", runtime)
            with self.assertRaisesRegex(RuntimeError, "OBS binary directory"):
                windows.require_obs_peer_directory(
                    prefix / "bin/windows-obs-peer.exe", runtime
                )
            with mock.patch.object(windows.subprocess, "Popen") as popen:
                child = windows.Child(["peer"], prefix / "peer.log", {}, cwd=runtime)
                child.log_file.close()
            self.assertEqual(popen.call_args.kwargs["cwd"], runtime)
            source = (ROOT / "tests/obs/run_windows_smoke.py").read_text()
            self.assertEqual(source.count("cwd=obs_runtime,"), 2)

    def test_windows_reference_live_replay_waits_for_decoded_media(self):
        reference = (ROOT / "tests/obs/windows_reference_peer.c").read_text()
        self.assertIn("Sleep(15);", reference)
        self.assertIn('printf("QUEUED %llu\\n"', reference)
        smoke = (ROOT / "tests/obs/run_windows_smoke.py").read_text()
        queued = smoke.index('"QUEUED" in read(reference_log)')
        decoded = smoke.index('"decoded encrypted A/V"')
        closed = smoke.index('sender.command("quit")', decoded)
        self.assertLess(queued, decoded)
        self.assertLess(decoded, closed)

    def test_windows_synthetic_video_uses_obs_clock_and_must_move(self):
        peer = (ROOT / "tests/obs/windows_obs_peer.c").read_text()
        self.assertIn("emit_synthetic(source, os_gettime_ns(), frame_index++);", peer)
        self.assertNotIn("now * UINT64_C(1000000)", peer)
        self.assertLess(
            peer.index("obs_source_filter_add(source, filter);"),
            peer.index("if (!sending)\n        obs_source_add_audio_capture_callback"),
        )
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "native-obs.log"
            log.write_text("MEDIA video=20 changed=10 audio=0 audible=0 bytes=500000\n")
            windows.require_video_motion(log)
            log.write_text("MEDIA video=20 changed=1 audio=0 audible=0 bytes=500000\n")
            with self.assertRaisesRegex(RuntimeError, "did not move"):
                windows.require_video_motion(log)

    def test_desktop_lifecycle_fix_is_pinned_idempotent_and_rejects_partial_edits(self):
        # Independently authored minimal fixture, not copied upstream source.
        original = (
            b"bool ffmpeg_mpegts_data_init(void) {\n"
            + lifecycle.RESET
            + b"\n}\nstatic bool set_config(void) {\nfail:\nreturn false;\n}\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / lifecycle.TARGET
            target.parent.mkdir(parents=True)
            target.write_bytes(original)
            with mock.patch.object(
                lifecycle, "ORIGINAL_SHA256", hashlib.sha256(original).hexdigest()
            ):
                target.write_bytes(original.replace(b"\n", b"\r\n"))
                self.assertTrue(lifecycle.prepare(root))
                fixed = target.read_bytes()
                self.assertIn(lifecycle.RELEASE, fixed)
                self.assertIn(lifecycle.CLEAN_FAILURE, fixed)
                self.assertNotIn(lifecycle.RESET, fixed)
                self.assertFalse(lifecycle.prepare(root))
                target.write_bytes(
                    fixed.replace(lifecycle.CLEAN_FAILURE, lifecycle.FAIL)
                )
                with self.assertRaisesRegex(RuntimeError, "partially applied"):
                    lifecycle.prepare(root)
                target.write_bytes(fixed + b"unrelated change\n")
                with self.assertRaisesRegex(RuntimeError, "unrecognized"):
                    lifecycle.prepare(root)
                self.assertEqual(target.read_bytes(), fixed + b"unrelated change\n")

    def test_desktop_checkpoint_reads_only_fresh_bounded_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            artifacts = Path(directory)
            output = artifacts / "output.ts"
            output.write_bytes(b"old" + b"n" * (3 * 1024 * 1024))
            child = mock.Mock()
            child.poll.return_value = None
            with mock.patch.object(desktop.media, "decoded_file") as decode:
                desktop.wait_decoded(
                    child, child, Path("/ffmpeg"), output, artifacts, "fresh", offset=3
                )
            decode.assert_called_once()
            self.assertEqual(
                (artifacts / "fresh-snapshot.ts").read_bytes(), b"n" * (2 * 1024 * 1024)
            )

    def test_desktop_reconnect_is_opt_in(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for enabled in (False, True):
                profile = root / str(enabled)
                desktop.prepare_profile(
                    profile, "/fixture.ts", "srt://localhost:9000", reconnect=enabled
                )
                settings = (
                    profile / "config/obs-studio/basic/profiles/Robotweax/basic.ini"
                ).read_text()
                self.assertIn(f"Reconnect={str(enabled).lower()}\n", settings)
                self.assertIn("RetryDelay=1\nMaxRetries=60\n", settings)

    def test_desktop_checkpoint_rejects_old_media(self):
        with tempfile.TemporaryDirectory() as directory:
            artifacts = Path(directory)
            output = artifacts / "output.ts"
            output.write_bytes(b"x" * 500000)
            child = mock.Mock()
            child.poll.return_value = None
            with mock.patch.object(desktop.time, "sleep"), mock.patch.object(
                desktop.time, "monotonic", side_effect=[0, 0, 16]
            ), mock.patch.object(desktop.media, "decoded_file") as decode:
                with self.assertRaisesRegex(RuntimeError, "decoded-media deadline"):
                    desktop.wait_decoded(
                        child,
                        child,
                        Path("/ffmpeg"),
                        output,
                        artifacts,
                        "fresh",
                        offset=500000,
                    )
            decode.assert_not_called()

    def test_desktop_media_wait_does_not_accept_transport_bytes_without_motion(self):
        with tempfile.TemporaryDirectory() as directory:
            artifacts = Path(directory)
            output = artifacts / "output.ts"
            output.write_bytes(b"x" * 500000)
            child = mock.Mock()
            child.poll.return_value = None
            with mock.patch.object(desktop.time, "sleep"), mock.patch.object(
                desktop.time, "monotonic", side_effect=[0, 0, 1]
            ), mock.patch.object(
                desktop.media, "decoded_file", side_effect=[RuntimeError("black"), None]
            ) as decode:
                desktop.wait_decoded(
                    child, child, Path("/ffmpeg"), output, artifacts, "test"
                )
            self.assertEqual(decode.call_count, 2)
            with mock.patch.object(desktop.time, "sleep"), mock.patch.object(
                desktop.time, "monotonic", side_effect=[0, 0, 16]
            ), mock.patch.object(
                desktop.media, "decoded_file", side_effect=RuntimeError("black")
            ):
                with self.assertRaisesRegex(RuntimeError, "decoded-media deadline"):
                    desktop.wait_decoded(
                        child, child, Path("/ffmpeg"), output, artifacts, "test"
                    )

    def test_desktop_shutdown_requires_normal_exit_and_no_allocation_regression(self):
        clean = "Freeing OBS context data\nNumber of memory leaks: 0\n"
        observed = clean.replace("leaks: 0", "leaks: 1")
        self.assertEqual(desktop.check_shutdown(0, clean), 0)
        self.assertEqual(desktop.check_shutdown(0, observed), 1)
        self.assertEqual(desktop.check_shutdown(0, observed, 1), 1)
        for status, text, baseline in (
            (1, clean, 0),
            (0, "", 0),
            (0, observed, 0),
            (0, observed.replace("leaks: 1", "leaks: 2"), None),
            (0, observed.replace("leaks: 1", "leaks: 2"), 1),
            (0, clean + clean, 0),
        ):
            with self.subTest(status=status, text=text, baseline=baseline):
                with self.assertRaises(RuntimeError):
                    desktop.check_shutdown(status, text, baseline)

    def test_desktop_preparation_is_separate_and_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            target = source / "plugins/CMakeLists.txt"
            target.parent.mkdir()
            original = b"synthetic original\n"
            target.write_bytes(original)
            with mock.patch.object(
                prepare, "ORIGINAL_SHA256", hashlib.sha256(original).hexdigest()
            ):
                self.assertTrue(prepare.prepare(source, "desktop"))
                self.assertEqual(target.read_bytes(), prepare.DESKTOP_PROFILE)
                self.assertFalse(prepare.prepare(source, "desktop"))
                with self.assertRaises(RuntimeError):
                    prepare.prepare(source, "headless")
            self.assertEqual(target.read_bytes(), prepare.DESKTOP_PROFILE)
            self.assertIn(b"add_subdirectory(rtmp-services)", prepare.DESKTOP_PROFILE)
            self.assertIn(b"add_subdirectory(obs-transitions)", prepare.DESKTOP_PROFILE)
            self.assertNotIn(b"rtmp-services", prepare.PROFILE)

    def test_desktop_fixture_profile_never_overwrites_existing_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "isolated"
            desktop.prepare_profile(root, "/fixture.ts", "srt://127.0.0.1:9000")
            config = root / "config/obs-studio"
            scenes = json.loads((config / "basic/scenes/Robotweax.json").read_text())
            source = scenes["sources"][0]
            self.assertEqual(source["id"], "ffmpeg_source")
            self.assertTrue(source["settings"]["is_local_file"])
            service = config / "basic/profiles/Robotweax/service.json"
            before = service.read_bytes()
            self.assertEqual(json.loads(before)["type"], "rtmp_custom")
            with self.assertRaises(FileExistsError):
                desktop.prepare_profile(root, "/other.ts", "srt://other")
            self.assertEqual(service.read_bytes(), before)

    def test_desktop_network_source_and_environment_are_isolated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "isolated"
            desktop.prepare_profile(root, "srt://source", "srt://dest", network=True)
            scene = root / "config/obs-studio/basic/scenes/Robotweax.json"
            settings = json.loads(scene.read_text())["sources"][0]["settings"]
            self.assertFalse(settings["is_local_file"])
            self.assertEqual(settings["input"], "srt://source")
            with mock.patch.dict(
                os.environ,
                {
                    "LD_PRELOAD": "/unexpected.so",
                    "QT_PLUGIN_PATH": "/unexpected",
                    "OBS_PLUGINS_PATH": "/unexpected",
                    "XDG_CONFIG_HOME": "/real-user",
                    "HOME": "/real-home",
                    "DISPLAY": ":99",
                },
                clear=True,
            ):
                command, environment = desktop.desktop_command(Path("/obs"), root)
            self.assertEqual(command[0], "/obs/bin/obs")
            self.assertEqual(environment["DISPLAY"], ":99")
            self.assertEqual(environment["HOME"], str(root / "home"))
            self.assertEqual(environment["XDG_CONFIG_HOME"], str(root / "config"))
            for variable in ("LD_PRELOAD", "QT_PLUGIN_PATH", "OBS_PLUGINS_PATH"):
                self.assertNotIn(variable, environment)

    def test_obs_version_does_not_depend_on_tags_in_shallow_checkout(self):
        script = (ROOT / "tests/obs/configure.sh").read_text()
        # Inspect the actual configure command, not comments or unused variables.
        command = shlex.split("cmake " + script.split("\ncmake ", 1)[1])
        versions = [arg for arg in command if arg.startswith("-DOBS_VERSION_OVERRIDE=")]
        self.assertEqual(
            versions, ["-DOBS_VERSION_OVERRIDE=32.2.2-robotweax-qualification"]
        )
        self.assertIn(
            '"$(git -C "$source_directory" rev-parse HEAD)" == '
            "ba2f32bdf791005443988a4955e963663e16b1ed",
            script,
        )

    def test_preparation_is_hash_guarded_and_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            target = source / "plugins/CMakeLists.txt"
            target.parent.mkdir()
            original = b"synthetic upstream build selection\n"
            target.write_bytes(original)
            with self.assertRaisesRegex(RuntimeError, "unrecognized"):
                prepare.prepare(source)
            self.assertEqual(target.read_bytes(), original)
            with mock.patch.object(
                prepare, "ORIGINAL_SHA256", hashlib.sha256(original).hexdigest()
            ):
                target.write_bytes(original.replace(b"\n", b"\r\n"))
                self.assertTrue(prepare.prepare(source))
                self.assertEqual(target.read_bytes(), prepare.PROFILE)
                with mock.patch.object(Path, "write_bytes") as write:
                    self.assertFalse(prepare.prepare(source))
                    write.assert_not_called()

    def test_media_requires_video_diversity_and_audible_audio(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "media.log"
            log.write_text(
                "MEDIA video=40 changed=30 audio=80 audible=60 bytes=90000\n"
            )
            obs.require_media(log)
            for evidence in (
                "",
                "MEDIA video=40 changed=1 audio=80 audible=60 bytes=90000\n",
                "MEDIA video=40 changed=30 audio=80 audible=0 bytes=90000\n",
                "MEDIA video=1 changed=1 audio=80 audible=60 bytes=90000\n",
            ):
                log.write_text(evidence)
                with self.assertRaisesRegex(RuntimeError, "insufficient"):
                    obs.require_media(log)

    def test_both_media_paths_require_one_expected_runtime_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "maps.log"
            library = root / "srt/lib/librobotweax-srt.so"
            library.parent.mkdir(parents=True)
            library.symlink_to("librobotweax-srt.so.0")
            mappings = (
                f"MAP 001-002 r-xp 0 0 1 {root}/srt/lib/librobotweax-srt.so.0\n"
                f"MAP 003-004 r-xp 0 0 2 {root}/ffmpeg/lib/libavformat.so.62\n"
                f"MAP 005-006 r-xp 0 0 3 {root}/obs/lib/obs-plugins/obs-ffmpeg.so\n"
                f"BINDING native {library}\nBINDING ffmpeg {library}\n"
            )
            log.write_text(mappings * 2)
            obs.require_provider(log, root / "obs", root / "srt", root / "ffmpeg")
            for content in (
                mappings + "MAP 009-010 r-xp 0 0 4 /usr/lib/libsrt-gnutls.so.1.5\n",
                mappings.replace(f"{root}/ffmpeg", "/unexpected"),
                mappings.replace(
                    f"BINDING ffmpeg {library}", "BINDING ffmpeg /unexpected/libsrt.so"
                ),
                mappings.splitlines()[0] + "\n",
            ):
                log.write_text(content)
                with self.assertRaises(RuntimeError):
                    obs.require_provider(
                        log, root / "obs", root / "srt", root / "ffmpeg"
                    )

    def test_commands_preserve_provider_environment_and_select_real_input(self):
        base = {"KEEP": "yes"}
        provider = (Path("/test/peer"), base)
        command, env = obs.obs_command(
            provider,
            Path("/obs"),
            "srt://source",
            "srt://dest?passphrase=valid&streamid=allowed&mode=caller",
            encrypted=True,
            wrong_key=True,
            streamid="denied",
        )
        self.assertEqual(command[-1], "network")
        self.assertEqual(command[-2], "srt://dest?mode=caller")
        self.assertNotEqual(env["ROBOTWEAX_OBS_PASSPHRASE"], obs.SECRET)
        self.assertEqual(env["ROBOTWEAX_OBS_STREAMID"], "denied")
        self.assertEqual(base, {"KEEP": "yes"})
        _, plain = obs.obs_command(provider, Path("/obs"), "fixture", local=True)
        self.assertNotIn("ROBOTWEAX_OBS_PASSPHRASE", plain)

    def test_build_keeps_native_mpegts_and_both_explicit_srt_paths(self):
        script = (ROOT / "tests/obs/configure.sh").read_text()
        self.assertIn("-DENABLE_NEW_MPEGTS_OUTPUT=ON", script)
        self.assertIn('-DLibsrt_LIBRARY="$srt_prefix/lib/librobotweax-srt.so"', script)
        self.assertIn("-DLibsrt_INCLUDE_DIR=", script)
        self.assertIn("libavformat", script)
        self.assertNotIn(
            "--disable-avformat", (ROOT / "tests/obs/configure_ffmpeg.sh").read_text()
        )
        ffmpeg = (ROOT / "tests/obs/configure_ffmpeg.sh").read_text()
        self.assertIn("--enable-zlib", ffmpeg)
        self.assertIn("wrapped_avframe,png", ffmpeg)
        self.assertIn("--enable-demuxer=mpegts,image2,image2pipe", ffmpeg)
        builder = (ROOT / "tests/obs/build_and_test.sh").read_text()
        self.assertEqual(
            builder.count('REVISION="$(<"$ffmpeg_source/RELEASE")-3acec0a"'), 2
        )

    def test_windows_build_selects_one_shared_compatibility_provider(self):
        script = (ROOT / "tests/obs/build_windows.ps1").read_text()
        self.assertIn("-DBUILD_SHARED_LIBS=ON", script)
        self.assertIn("-DROBOTWEAX_SRT_INSTALL_LAYOUT=legacy", script)
        self.assertIn("-DROBOTWEAX_SRT_CRYPTO_BACKEND=bcrypt", script)
        self.assertIn("-DENABLE_FRONTEND=OFF", script)
        self.assertNotIn("-DENABLE_UI=OFF", script)
        self.assertIn("-DLibsrt_LIBRARY:FILEPATH=$SrtLibrary", script)
        self.assertIn("Get-FileHash $RobotweaxDll", script)
        self.assertIn("Get-FileHash $ReferenceDll", script)
        self.assertIn("Where-Object { $_.Name -cne 'srt.dll' }", script)
        self.assertIn('Copy-Item $RobotweaxDll "$RuntimeDirectory/srt.dll"', script)
        self.assertIn("'--component', 'Development'", script)
        self.assertIn('--ffmpeg-cli $FfmpegCli', script)
        self.assertIn('$ObsPeer = "$ObsPrefix/bin/64bit/windows-obs-peer.exe"', script)
        obs_build = script.split("'-S', $ObsSource", 1)[1].split(
            "Invoke-Checked cmake @('--install', $ObsBuild", 1
        )[0]
        self.assertNotIn("'--target'", obs_build)

    def test_windows_desktop_build_and_required_ci_job(self):
        script = (ROOT / "tests/obs/build_windows.ps1").read_text()
        self.assertIn("[switch]$Desktop", script)
        self.assertIn("prepare_desktop_lifecycle.py", script)
        self.assertIn("'--profile', 'desktop'", script)
        self.assertIn("-DENABLE_FRONTEND=ON", script)
        self.assertIn("run_windows_desktop_smoke.py", script)
        self.assertIn("$ObsPrefix/bin/64bit/obs64.exe", script)
        self.assertIn("if ($Preview -and !$Desktop)", script)
        self.assertIn("package_windows_preview.py", script)
        self.assertIn('"$WorkDirectory/preview/MANIFEST.json"', script)
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        job = workflow.split("  obs_windows_desktop:\n", 1)[1].split(
            "  vlc_integration:\n", 1
        )[0]
        self.assertIn("-Desktop", job)
        self.assertNotIn("-Preview", job)
        self.assertNotIn("package_windows_preview.py", job)
        self.assertIn("ref: ${{ env.OBS_COMMIT }}", job)
        artifact_paths = job.split("          path: |\n", 1)[1].split(
            "          retention-days:", 1
        )[0]
        self.assertIn("evidence/*.txt", artifact_paths)
        self.assertNotIn("preview-", artifact_paths)
        for binary_suffix in (".dll", ".exe", ".zip"):
            self.assertNotIn(binary_suffix, artifact_paths)
        required = workflow.split("  ci_gate:\n", 1)[1]
        self.assertIn("      - obs_windows_desktop\n", required)
        self.assertIn("${{ needs.obs_windows_desktop.result }}", required)
        self.assertIn('"obs-windows-desktop=$OBS_WINDOWS_DESKTOP"', required)

    def test_windows_peer_uses_valid_unbuffered_stdout(self):
        source = (ROOT / "tests/obs/windows_obs_peer.c").read_text()
        self.assertIn("setvbuf(stdout, NULL, _IONBF, 0);", source)
        self.assertNotIn("setvbuf(stdout, NULL, _IOLBF, 0);", source)

    def test_windows_obs_job_is_required_and_uploads_only_diagnostics(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        job = workflow.split("  obs_windows_integration:\n", 1)[1].split(
            "  vlc_integration:\n", 1
        )[0]
        self.assertIn("tests\\obs\\build_windows.ps1", job)
        paths = job.split("          path: |\n", 1)[1].split(
            "          retention-days:", 1
        )[0]
        self.assertTrue(
            all(
                "qualification" in line
                for line in paths.splitlines()
                if line.strip()
            )
        )
        self.assertNotIn(".dll", paths)
        self.assertNotIn(".exe", paths)
        required = workflow.split("  ci_gate:\n", 1)[1]
        self.assertIn("      - obs_windows_integration\n", required)
        self.assertIn(
            "${{ needs.obs_windows_integration.result }}", required
        )
        self.assertIn(
            '"obs-windows-integration=$OBS_WINDOWS_INTEGRATION"', required
        )

    def test_classification_selects_obs_without_unrelated_media_jobs(self):
        from interop.ci_changes import classify

        for path in (
            "tests/obs/configure.sh",
            "tests/obs/qualification.cmake",
            "tests/obs/peer.c",
            "tests/obs/run_smoke.py",
            "tests/obs/windows_obs_peer.c",
            "tests/obs/windows_reference_peer.c",
            "tests/obs/run_windows_smoke.py",
            "tests/obs/prepare_windows_source.py",
            "interop/tests/test_obs_harness.py",
        ):
            with self.subTest(path=path):
                result = classify([path])
                self.assertTrue(result.obs)
                self.assertFalse(result.vlc)
                self.assertFalse(result.gstreamer)
                self.assertEqual(result.format, path.endswith(".c"))
                self.assertEqual(result.python, path.endswith(".py"))
        for path in (
            "src/compat/readiness.cpp",
            "cmake/srt.pc.in",
            "tests/gstreamer/peer.c",
        ):
            self.assertTrue(classify([path]).obs)
        self.assertTrue(classify([], force_full=True).obs)

    def test_ci_uploads_diagnostics_not_obs_or_gpl_binaries(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        job = workflow.split("  obs_integration:\n", 1)[1].split(
            "  vlc_integration:\n", 1
        )[0]
        paths = job.split("          path: |\n", 1)[1].split(
            "          retention-days:", 1
        )[0]
        self.assertEqual(
            [line.strip() for line in paths.splitlines()],
            [
                "${{ runner.temp }}/obs-qualification/evidence/*.log",
                "${{ runner.temp }}/obs-qualification/evidence/*.txt",
                "${{ runner.temp }}/obs-qualification/evidence/*.frames",
                "${{ runner.temp }}/obs-desktop/evidence/*.log",
                "${{ runner.temp }}/obs-desktop/evidence/*.txt",
                "${{ runner.temp }}/obs-desktop/evidence/runtime/*.log",
                "${{ runner.temp }}/obs-desktop/evidence/runtime/*.txt",
                "${{ runner.temp }}/obs-desktop/evidence/runtime/*.frames",
                "${{ runner.temp }}/obs-desktop/evidence/runtime/*.jsonl",
            ],
        )
        self.assertIn("tests/obs/build_desktop.sh", job)
        self.assertIn("tests/obs/run_desktop_smoke.py", job)
        self.assertIn("--reconnect-cycles 3 --soak-seconds 30", job)
        self.assertNotIn("continue-on-error", job)


if __name__ == "__main__":
    unittest.main()
