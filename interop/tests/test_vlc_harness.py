from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "vlc_smoke", ROOT / "tests/vlc/run_smoke.py"
)
vlc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vlc)
prepare_spec = importlib.util.spec_from_file_location(
    "vlc_prepare", ROOT / "tests/vlc/prepare_source.py"
)
prepare = importlib.util.module_from_spec(prepare_spec)
prepare_spec.loader.exec_module(prepare)


class VlcHarnessTests(unittest.TestCase):
    def test_source_preparation_is_exact_and_idempotent(self):
        # Synthetic declaration, not a copy of the upstream source file.
        before = b"prefix\n\tadd_obsolete_integer(SRT_PARAM_PAYLOAD_SIZE)\nsuffix\n"
        after = b"prefix\nsuffix\n"
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            target = source / "modules/access/srt.c"
            target.parent.mkdir(parents=True)
            target.write_bytes(before)
            with mock.patch.object(
                prepare, "ORIGINAL_SHA256", hashlib.sha256(before).hexdigest()
            ), mock.patch.object(
                prepare, "PREPARED_SHA256", hashlib.sha256(after).hexdigest()
            ):
                self.assertTrue(prepare.prepare(source))
                self.assertEqual(target.read_bytes(), after)
                with mock.patch.object(Path, "write_bytes") as write:
                    self.assertFalse(prepare.prepare(source))
                    write.assert_not_called()

    def test_source_preparation_rejects_unknown_or_modified_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            target = source / "modules/access/srt.c"
            target.parent.mkdir(parents=True)
            target.write_bytes(b"unrecognized source\n")
            with self.assertRaisesRegex(RuntimeError, "unrecognized"):
                prepare.prepare(source)
            self.assertEqual(target.read_bytes(), b"unrecognized source\n")

    def test_source_preparation_checks_replacement_count_and_digest(self):
        declaration = b"add_obsolete_integer(SRT_PARAM_PAYLOAD_SIZE)\n"
        for before, after in ((declaration * 2, b""), (declaration, b"wrong")):
            with self.subTest(
                before=before
            ), tempfile.TemporaryDirectory() as directory:
                source = Path(directory)
                target = source / "modules/access/srt.c"
                target.parent.mkdir(parents=True)
                target.write_bytes(before)
                with mock.patch.object(
                    prepare, "ORIGINAL_SHA256", hashlib.sha256(before).hexdigest()
                ), mock.patch.object(
                    prepare, "PREPARED_SHA256", hashlib.sha256(after).hexdigest()
                ), self.assertRaisesRegex(
                    RuntimeError, "unexpected"
                ):
                    prepare.prepare(source)
                self.assertEqual(target.read_bytes(), before)

    def test_configure_prepares_source_before_bootstrap(self):
        helper = (ROOT / "tests/vlc/configure.sh").read_text()
        self.assertLess(helper.index("prepare_source.py"), helper.index("./bootstrap"))

    def test_frames_require_media_identity_and_real_input_module(self):
        frames = [f"{value:016x}" for value in range(20)]
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "decode.log"
            content = "\n".join("FRAME " + value for value in frames)
            log.with_suffix(".frames").write_text(content)
            log.write_text('using access module "access_srt"\n' + content)
            vlc.require_frames(log, set(frames))
            with self.assertRaisesRegex(RuntimeError, "differs"):
                vlc.require_frames(log, set(frames[:-1]))
            log.write_text(content)
            with self.assertRaisesRegex(RuntimeError, "actual VLC SRT"):
                vlc.require_frames(log, set(frames))
            log.write_text(
                'using access module "access_srt"\n' + "FRAME 0000000000000000\n" * 20
            )
            log.with_suffix(".frames").write_text("FRAME 0000000000000000\n" * 20)
            with self.assertRaisesRegex(RuntimeError, "diversity"):
                vlc.require_frames(log, set(frames))

    def test_sender_and_receiver_select_distinct_installed_modules(self):
        provider = (Path("/test/peer"), {"KEEP": "yes"}, Path("/test/vlc"))
        for mode, suffix in (
            ("send", "access_output/libaccess_output_srt_plugin.so"),
            ("decode", "access/libaccess_srt_plugin.so"),
        ):
            with self.subTest(mode=mode):
                command, environment = vlc.vlc_command(
                    provider, mode, "source", 20, True
                )
                self.assertEqual(command[1], mode)
                self.assertEqual(
                    environment["ROBOTWEAX_VLC_PLUGIN"],
                    "/test/vlc/lib/vlc/plugins/" + suffix,
                )
                self.assertEqual(
                    environment["ROBOTWEAX_VLC_TEST_PASSPHRASE"], vlc.SECRET
                )
        self.assertEqual(provider[1], {"KEEP": "yes"})

    def test_negative_secret_is_distinct_and_opt_in(self):
        provider = (Path("/test/peer"), {}, Path("/test/vlc"))
        _, plain = vlc.vlc_command(provider, "send", "source", "dest")
        _, bad = vlc.vlc_command(provider, "send", "source", "dest", True, True)
        self.assertNotIn("ROBOTWEAX_VLC_TEST_PASSPHRASE", plain)
        self.assertNotEqual(bad["ROBOTWEAX_VLC_TEST_PASSPHRASE"], vlc.SECRET)


if __name__ == "__main__":
    unittest.main()
