from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import tempfile
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


class ObsHarnessTests(unittest.TestCase):
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
        obs_build = script.split("'-S', $ObsSource", 1)[1].split(
            "Invoke-Checked cmake @('--install', $ObsBuild", 1
        )[0]
        self.assertNotIn("'--target'", obs_build)

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
