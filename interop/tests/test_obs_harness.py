from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
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


class ObsHarnessTests(unittest.TestCase):
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
        builder = (ROOT / "tests/obs/build_and_test.sh").read_text()
        self.assertEqual(
            builder.count('REVISION="$(<"$ffmpeg_source/RELEASE")-3acec0a"'), 2
        )

    def test_classification_selects_obs_without_unrelated_media_jobs(self):
        from interop.ci_changes import classify

        for path in (
            "tests/obs/configure.sh",
            "tests/obs/qualification.cmake",
            "tests/obs/peer.c",
            "tests/obs/run_smoke.py",
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
            ],
        )


if __name__ == "__main__":
    unittest.main()
