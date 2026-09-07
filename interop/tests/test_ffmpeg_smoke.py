"""Exercise smoke-harness cleanup without a codec build or network sockets."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SMOKE = Path(__file__).resolve().parents[2] / "tests/ffmpeg/run_smoke.sh"


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "POSIX shell")
class FFmpegSmokeCleanupTests(unittest.TestCase):
    def run_smoke(self, *, fail=False, keep=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        ffmpeg = root / "ffmpeg"
        ffmpeg.write_text(
            "#!/bin/sh\n"
            'if [ "$1" = "-version" ]; then echo fake-ffmpeg; exit 0; fi\n'
            + ("echo injected-fixture-failure >&2; exit 17\n" if fail else
               'for last do :; done\n'
               'case "$last" in srt://*) exit 0;; esac\n'
               'printf "synthetic payload" > "$last"\n'
               # Keep the fake listener alive until the caller-start check.
               'case "$*" in *"-i srt://"*) sleep 1;; esac\n'),
            encoding="utf-8",
        )
        ffmpeg.chmod(0o755)
        port_helper = root / "python3"
        port_helper.write_text("#!/bin/sh\necho 9000\n", encoding="utf-8")
        port_helper.chmod(0o755)
        # Bound any watchdog child inherited by capture_output after its shell
        # is stopped. Real FFmpeg timing is covered by the integration job.
        sleep_helper = root / "sleep"
        sleep_helper.write_text(
            '#!/bin/sh\nif [ "$1" = 15 ]; then exec /bin/sleep 1; fi\n'
            'exec /bin/sleep "$@"\n',
            encoding="utf-8",
        )
        sleep_helper.chmod(0o755)
        environment = dict(os.environ)
        environment.update(
            TMPDIR=str(root),
            PATH=str(root) + os.pathsep + environment.get("PATH", ""),
            ROBOTWEAX_FFMPEG_KEEP_ARTIFACTS="1" if keep else "0",
        )
        result = subprocess.run(
            ["bash", str(SMOKE), str(ffmpeg)],
            env=environment,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        return result, list(root.glob("robotweax-ffmpeg.*"))

    def test_failure_retains_evidence_and_original_exit_status(self):
        result, directories = self.run_smoke(fail=True)
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertEqual(len(directories), 1)
        self.assertEqual(
            (directories[0] / "ffmpeg-version.log").read_text(),
            "fake-ffmpeg\n",
        )
        self.assertIn("artifacts retained", result.stderr)

    def test_success_removes_temporary_files_by_default(self):
        result, directories = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(directories, [])

    def test_success_can_retain_evidence_explicitly(self):
        result, directories = self.run_smoke(keep=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(directories), 1)
        self.assertTrue((directories[0] / "actual-aes-ctr.m2v").is_file())


if __name__ == "__main__":
    unittest.main()
