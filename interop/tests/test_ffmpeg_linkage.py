"""Exercise the actual FFmpeg workflow linkage guard across ABI lines."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("bash") and shutil.which("pkg-config"),
                     "FFmpeg linkage guard requires bash and pkg-config")
class FfmpegLinkageTests(unittest.TestCase):
    def verify(self, abi: str, library: str, protocol: str = "srt"):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        step = workflow.split(
            "      - name: Verify Robotweax linkage and SRT registration\n", 1
        )[1].split("      - name:", 1)[0]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "robotweax-srt"
            metadata = package / "lib/pkgconfig"
            metadata.mkdir(parents=True)
            (metadata / "srt.pc").write_text(
                f"robotweax_abi={abi}\nName: Robotweax SRT test\n"
                "Description: isolated linkage test\nVersion: 1.5.7\n"
            )
            tools = root / "bin"
            tools.mkdir()
            ldd = tools / "ldd"
            ldd.write_text('#!/bin/sh\nprintf "%s\\n" "$TEST_LINKAGE"\n')
            ldd.chmod(0o700)
            ffmpeg = root / "ffmpeg-source/ffmpeg"
            ffmpeg.parent.mkdir()
            ffmpeg.write_text('#!/bin/sh\nprintf "  %s\\n" "$TEST_PROTOCOL"\n')
            ffmpeg.chmod(0o700)
            environment = {
                **os.environ, "RUNNER_TEMP": str(root),
                "PATH": str(tools) + os.pathsep + os.environ["PATH"],
                "TEST_PROTOCOL": protocol,
                "TEST_LINKAGE": "librobotweax-srt.so => "
                + library.format(prefix=package) + " (0x12345678)",
            }
            return subprocess.run(
                ["bash", "-c", script], cwd=root, env=environment,
                capture_output=True, text=True, timeout=5,
            )

    def test_accepts_installed_abi_independently_of_srt_api_version(self):
        for abi in ["0.2", "0.3"]:
            with self.subTest(abi=abi):
                result = self.verify(abi, "{prefix}/lib/librobotweax-srt.so." + abi)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_wrong_abi_and_prefix_matches(self):
        for library in [
            "{prefix}/lib/librobotweax-srt.so.0.2",
            "{prefix}/lib/librobotweax-srt.so.0.30",
            "/usr/lib/librobotweax-srt.so.0.3",
            "/usr/lib/libsrt.so.1.5",
        ]:
            with self.subTest(library=library):
                self.assertNotEqual(self.verify("0.3", library).returncode, 0)

    def test_rejects_empty_or_malformed_abi_metadata(self):
        for abi in ["", "0.3 unexpected", "1.5.7"]:
            with self.subTest(abi=abi):
                self.assertNotEqual(self.verify(
                    abi, "{prefix}/lib/librobotweax-srt.so.0.3"
                ).returncode, 0)

    def test_requires_srt_protocol_registration(self):
        self.assertNotEqual(self.verify(
            "0.3", "{prefix}/lib/librobotweax-srt.so.0.3", protocol="udp"
        ).returncode, 0)
