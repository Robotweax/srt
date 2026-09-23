from __future__ import annotations

from array import array
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
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
macos_cache = load("obs_macos_cache", "check_macos_cache.py")
macos = load("obs_macos_smoke", "run_macos_smoke.py")
macos_desktop = load("obs_macos_desktop", "run_macos_desktop_smoke.py")


def signed_macho(payload: bytes, signature: bytes = b"signature") -> bytes:
    """A minimal Mach-O fixture with independently variable signing data."""
    header = struct.pack("<IiiIIIII", 0xFEEDFACF, 0x100000C, 0, 6, 2, 88, 0, 0)
    segment = struct.pack(
        "<II16sQQQQiiII", 0x19, 72, b"__LINKEDIT", 0,
        len(payload) + len(signature), 120, len(payload) + len(signature),
        0, 0, 0, 0,
    )
    code_signature = struct.pack("<IIII", 0x1D, 16, 120 + len(payload), len(signature))
    return header + segment + code_signature + payload + signature


def universal_macho(arm64: bytes, *, fat64: bool = False) -> bytes:
    x86 = bytearray(signed_macho(b"unrelated x86 code"))
    struct.pack_into("<I", x86, 4, 0x01000007)
    entry_size = 32 if fat64 else 20
    first = 8 + 2 * entry_size
    second = first + len(x86)
    if fat64:
        entries = struct.pack(">IIQQII", 0x01000007, 0, first, len(x86), 0, 0)
        entries += struct.pack(">IIQQII", 0x0100000C, 0, second, len(arm64), 0, 0)
    else:
        entries = struct.pack(">IIIII", 0x01000007, 0, first, len(x86), 0)
        entries += struct.pack(">IIIII", 0x0100000C, 0, second, len(arm64), 0)
    magic = 0xCAFEBABF if fat64 else 0xCAFEBABE
    return struct.pack(">II", magic, 2) + entries + x86 + arm64


class ObsHarnessTests(unittest.TestCase):
    def test_macos_signed_macho_hash_ignores_only_signing_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "library.dylib"
            path.write_bytes(signed_macho(b"Robotweax", b"first"))
            expected = macos_desktop.macho_payload_sha256(path)
            path.write_bytes(signed_macho(b"Robotweax", b"different signature"))
            self.assertEqual(macos_desktop.macho_payload_sha256(path), expected)
            path.write_bytes(signed_macho(b"other code", b"first"))
            self.assertNotEqual(macos_desktop.macho_payload_sha256(path), expected)
            path.write_bytes(b"not a Mach-O")
            with self.assertRaisesRegex(RuntimeError, "Mach-O"):
                macos_desktop.macho_payload_sha256(path)

    def test_macos_universal_macho_hashes_only_arm64_code(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "universal.dylib"
            expected = signed_macho(b"pinned arm64 code", b"original signature")
            path.write_bytes(expected)
            original_hash = macos_desktop.macho_payload_sha256(path)
            for fat64 in (False, True):
                path.write_bytes(
                    universal_macho(
                        signed_macho(b"pinned arm64 code", b"resigned by Xcode"),
                        fat64=fat64,
                    )
                )
                self.assertEqual(
                    macos_desktop.macho_payload_sha256(path), original_hash
                )
                path.write_bytes(
                    universal_macho(signed_macho(b"different arm64 code"), fat64=fat64)
                )
                self.assertNotEqual(
                    macos_desktop.macho_payload_sha256(path), original_hash
                )
            path.write_bytes(struct.pack(">II", 0xCAFEBABE, 2))
            with self.assertRaisesRegex(RuntimeError, "architecture table"):
                macos_desktop.macho_payload_sha256(path)

    def test_macos_desktop_uses_private_cocoa_profile_and_rejects_reuse(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "desktop"
            fixture = Path(directory) / "fixture.ts"
            config = macos_desktop.prepare_profile(root, fixture, "srt://output")
            command, env = macos_desktop.desktop_command(
                Path(directory) / "OBS", config, stream=True
            )
            self.assertEqual(env["HOME"], str(root / "home"))
            self.assertEqual(env["CFFIXED_USER_HOME"], env["HOME"])
            self.assertIn("--startstreaming", command)
            self.assertTrue(config.is_relative_to(root))
            self.assertIn(
                "MacOSPermissionsDialogLastShown=1\n",
                (config / "global.ini").read_text(),
            )
            self.assertEqual(
                json.loads((config / "basic/scenes/Robotweax.json").read_text())
                ["sources"][0]["settings"]["local_file"],
                str(fixture),
            )
            with self.assertRaises(FileExistsError):
                macos_desktop.prepare_profile(root, fixture, "srt://other")
            network = macos_desktop.prepare_profile(
                Path(directory) / "source", fixture, "srt://output", source="srt://input"
            )
            settings = json.loads(
                (network / "basic/scenes/Robotweax.json").read_text()
            )["sources"][0]["settings"]
            self.assertFalse(settings["is_local_file"])
            self.assertEqual(settings["input"], "srt://input")

    def test_macos_desktop_window_requires_initialized_scenes(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory)
            logs = config / "logs"
            logs.mkdir()
            log = logs / "obs.txt"
            with mock.patch.object(macos_desktop, "visible_window", return_value=True):
                self.assertFalse(
                    macos_desktop.ready_window(None, None, None, config)
                )
                log.write_text("Permissions dialog opened\n")
                self.assertFalse(
                    macos_desktop.ready_window(None, None, None, config)
                )
                log.write_text("Loaded scenes:\n")
                self.assertTrue(macos_desktop.ready_window(None, None, None, config))

    def test_macos_desktop_requires_exact_embedded_provider_and_normal_shutdown(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = root / "build/frontend/Release/OBS.app"
            for relative in (
                "Contents/MacOS/OBS",
                "Contents/PlugIns/obs-ffmpeg.plugin/Contents/MacOS/obs-ffmpeg",
                "Contents/PlugIns/rtmp-services.plugin/Contents/MacOS/rtmp-services",
            ):
                target = app / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"OBS")
            srt = root / "srt/lib/librobotweax-srt.dylib"
            ffmpeg = root / "ffmpeg/lib/libavformat.dylib"
            crypto = root / "deps/lib/libmbedcrypto.dylib"
            for target, content in (
                (srt, b"Robotweax"),
                (ffmpeg, b"FFmpeg"),
                (crypto, b"pinned crypto"),
            ):
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(signed_macho(content, b"original"))
                embedded = app / "Contents/Frameworks" / target.name
                embedded.parent.mkdir(parents=True, exist_ok=True)
                embedded.write_bytes(signed_macho(content, b"resigned by Xcode"))
            args = SimpleNamespace(obs_build=root / "build", srt_prefix=root / "srt",
                                   ffmpeg_prefix=root / "ffmpeg", reference_prefix=root / "deps")
            self.assertEqual(
                macos_desktop.bundle_contract(args), app / "Contents/MacOS/OBS"
            )
            (app / "Contents/Frameworks/libmbedcrypto.dylib").write_bytes(
                signed_macho(b"wrong crypto")
            )
            with self.assertRaisesRegex(RuntimeError, "Librist crypto"):
                macos_desktop.bundle_contract(args)
            (app / "Contents/Frameworks/libmbedcrypto.dylib").write_bytes(
                signed_macho(b"pinned crypto", b"resigned by Xcode")
            )
            (app / "Contents/Frameworks/librobotweax-srt.dylib").write_bytes(
                signed_macho(b"wrong")
            )
            with self.assertRaisesRegex(RuntimeError, "non-Robotweax"):
                macos_desktop.bundle_contract(args)
            (app / "Contents/Frameworks/librobotweax-srt.dylib").write_bytes(
                signed_macho(b"Robotweax", b"resigned by Xcode")
            )
            (app / "Contents/Frameworks/libsrt.dylib").write_bytes(b"competing")
            with self.assertRaisesRegex(RuntimeError, "competing"):
                macos_desktop.bundle_contract(args)

        log = "Loaded scenes:\n==== Shutting down\nFreeing OBS context data\n"
        self.assertEqual(
            macos_desktop.check_shutdown(0, log, "Number of memory leaks: 1\n"), 1
        )
        with self.assertRaisesRegex(RuntimeError, "allocation regression"):
            macos_desktop.check_shutdown(0, log, "Number of memory leaks: 2\n", 1)
        with self.assertRaisesRegex(RuntimeError, "normally"):
            macos_desktop.check_shutdown(1, log, "Number of memory leaks: 1\n")
        with self.assertRaisesRegex(RuntimeError, "streaming shutdown"):
            macos_desktop.check_shutdown(
                0, log, "Number of memory leaks: 1\n", 1, streamed=True
            )

    def test_macos_desktop_runtime_mapping_rejects_other_srt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            srt = root / "srt/lib/librobotweax-srt.dylib"
            avformat = root / "ffmpeg/lib/libavformat.dylib"
            plugin = root / "obs-ffmpeg.plugin/Contents/MacOS/obs-ffmpeg"
            other = root / "other/libsrt.dylib"
            for path in (srt, avformat, plugin, other):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(signed_macho(path.name.encode()))
            args = SimpleNamespace(srt_prefix=root / "srt", ffmpeg_prefix=root / "ffmpeg")
            lines = f"__TEXT  {srt}\n__TEXT  {avformat}\n__TEXT  {plugin}\n"
            result = SimpleNamespace(returncode=0, stdout=lines, stderr="")
            evidence = root / "mappings.txt"
            with mock.patch.object(macos_desktop.subprocess, "run", return_value=result):
                macos_desktop.verify_maps(SimpleNamespace(pid=1234), args, evidence)
                self.assertIn(str(srt), evidence.read_text())
                result.stdout += f"__TEXT  {other}\n"
                with self.assertRaisesRegex(RuntimeError, "competing SRT"):
                    macos_desktop.verify_maps(SimpleNamespace(pid=1234), args, evidence)

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("cc"), "requires macOS compiler")
    def test_macos_window_probe_compiles_and_rejects_invalid_pid(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "window-probe"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests/obs/macos_window_probe.c"),
                 "-framework", "CoreGraphics", "-framework", "CoreFoundation",
                 "-o", str(binary)],
                check=True, capture_output=True,
            )
            result = subprocess.run(
                [str(binary), "0"], capture_output=True, text=True
            )
            self.assertEqual(result.returncode, 2, result.stderr)

    def test_macos_desktop_ci_keeps_binary_out_of_uploaded_artifacts(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        job = workflow.split("  obs_macos_desktop:\n", 1)[1].split(
            "  vlc_integration:\n", 1
        )[0]
        self.assertIn("tests/obs/build_macos.sh", job)
        self.assertIn(" desktop\n", job)
        self.assertIn("runs-on: macos-26", job)
        paths = job.split("          path: |\n", 1)[1].split(
            "          retention-days:", 1
        )[0]
        self.assertNotIn(".app", paths)
        self.assertNotIn(".dylib", paths)
        self.assertNotIn(".plugin", paths)
        self.assertIn("      - obs_macos_desktop\n", workflow)

    @unittest.skipUnless(shutil.which("cc"), "requires a C compiler")
    def test_reference_pacing_handles_late_wakeups_without_unbounded_bursts(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "pacing-test"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests/obs/test_reference_pacing.c"), "-o", str(binary)],
                check=True, capture_output=True,
            )
            subprocess.run([str(binary)], check=True, capture_output=True)

    def test_windows_provider_records_are_isolated_from_runtime_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            provider = root / "srt.dll"
            records = root / "evidence.log"
            runtime = root / "runtime.log"
            content = (
                f"MODULE {provider}\nMODULE {root / 'obs-ffmpeg.dll'}\n"
                f"MODULE {root / 'avformat-62.dll'}\n"
                f"BINDING {provider}\nBINDING {provider}\n"
                "MEDIA video=30 changed=25 audio=40 audible=35 bytes=500000\n"
                "SHUTDOWN\n"
            )
            records.write_text("stale evidence")
            env = dict(os.environ)
            script = (
                "import os, pathlib, sys; "
                "records = pathlib.Path(os.environ['ROBOTWEAX_OBS_EVIDENCE']); "
                "assert records.read_text() == ''; "
                "print('BINDING bad.dllwarning: concurrent OBS log', flush=True); "
                "records.write_text(sys.argv[1])"
            )
            child = windows.Child(
                [sys.executable, "-c", script, content], runtime, env, evidence=records
            )
            try:
                child.finish()
            finally:
                child.stop()
                child.process.stdin.close()
            self.assertEqual(env, dict(os.environ))
            self.assertIn("concurrent OBS log", runtime.read_text())
            windows.require_provider(records, provider)
            windows.require_media(records)
            records.write_text(
                content.replace(f"BINDING {provider}\n", "BINDING bad.dll\n", 1)
            )
            with self.assertRaisesRegex(RuntimeError, "unexpected SRT bindings"):
                windows.require_provider(records, provider)
            with self.assertRaisesRegex(ValueError, "separate"):
                windows.Child(["unused"], runtime, env, evidence=runtime)

    def test_macos_runtime_paths_ignore_xcode_linker_stub(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            binaries = (
                "libobs/Release/libobs.framework/Versions/A/libobs",
                "plugins/obs-ffmpeg/Release/obs-ffmpeg.plugin/Contents/MacOS/obs-ffmpeg",
                "plugins/obs-x264/Release/obs-x264.plugin/Contents/MacOS/obs-x264",
                "libobs-opengl/Release/libobs-opengl.dylib",
            )
            for relative in binaries:
                binary = build / relative
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(b"fixture")
            stub = build / "build/EagerLinkingTBDs/Release/libobs.framework"
            stub.mkdir(parents=True)
            framework, ffmpeg_plugin, x264_plugin, graphics = macos.runtime_paths(build)
            self.assertEqual(framework, build / "libobs/Release/libobs.framework")
            self.assertEqual(ffmpeg_plugin, build / "plugins/obs-ffmpeg/Release/obs-ffmpeg.plugin")
            self.assertEqual(x264_plugin, build / "plugins/obs-x264/Release/obs-x264.plugin")
            self.assertEqual(graphics, build / "libobs-opengl/Release/libobs-opengl.dylib")
            (build / binaries[1]).unlink()
            with self.assertRaisesRegex(RuntimeError, "missing OBS runtime binary"):
                macos.runtime_paths(build)

    def test_macos_stalled_source_collects_text_stack_sample(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "source-obs-stacks.txt"
            with mock.patch.object(macos.subprocess, "run") as run:
                run.return_value.returncode = 0
                macos.sample_process(SimpleNamespace(pid=1234), destination)
            self.assertEqual(
                run.call_args.args[0],
                ["sample", "1234", "2", "-file", str(destination)],
            )
            self.assertTrue(destination.is_file())
            self.assertIn("sample exit=0", destination.read_text())
        smoke = (ROOT / "tests/obs/run_macos_smoke.py").read_text()
        self.assertIn(
            'sample_process(source, artifacts / "source-reference-stacks.txt")',
            smoke,
        )
        reference = (ROOT / "tests/obs/macos_reference_peer.c").read_text()
        self.assertIn('printf("SENT %llu\\n"', reference)

    def test_macos_native_shutdown_timeout_collects_text_stack_sample(self):
        smoke = (ROOT / "tests/obs/run_macos_smoke.py").read_text()
        self.assertIn(
            'except subprocess.TimeoutExpired:\n'
            '                sample_process(child, artifacts / "native-obs-stacks.txt")',
            smoke,
        )
        peer = (ROOT / "tests/obs/peer.c").read_text()
        stop = peer.index('puts("TEARDOWN stop-output")')
        release = peer.index('puts("TEARDOWN release-output")')
        shutdown = peer.index('puts("TEARDOWN obs-shutdown")')
        self.assertLess(stop, release)
        self.assertLess(release, shutdown)
        self.assertLess(shutdown, peer.index("obs_shutdown();"))

    def test_macos_live_source_uses_bounded_ffmpeg_probe(self):
        peer = (ROOT / "tests/obs/peer.c").read_text()
        self.assertIn('if (!local)\n        obs_data_set_string(settings, "ffmpeg_options",', peer)
        self.assertIn('"probesize=131072 analyzeduration=3000000"', peer)
        smoke = (ROOT / "tests/obs/run_macos_smoke.py").read_text()
        self.assertIn("if video < 20 or changed < 10 or audio < 20 or audible < 10:", smoke)
        queued = smoke.index('until(source, sender_log, "QUEUED", 35)')
        observed = smoke.index("require_media(obs_log)", queued)
        closed = smoke.index('tell(source, "quit")', observed)
        self.assertLess(queued, observed)
        self.assertLess(observed, closed)

    def test_macos_cache_rejects_prebuilt_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ffmpeg = root / "ffmpeg"
            srt = root / "srt"
            cache = root / "CMakeCache.txt"
            values = {
                "Libsrt_LIBRARY": srt / "lib/librobotweax-srt.dylib",
                "Libsrt_INCLUDE_DIR": srt / "include",
            }
            for component in ("avcodec", "avdevice", "avfilter", "avformat",
                              "avutil", "swscale", "swresample"):
                values[f"FFmpeg_{component}_LIBRARY"] = ffmpeg / "lib" / f"lib{component}.dylib"
                values[f"FFmpeg_{component}_INCLUDE_DIR"] = ffmpeg / "include"

            def write_cache():
                cache.write_text("".join(
                    f"\n// Generated CMake comment for {key}\n{key}:FILEPATH={value}\n"
                    for key, value in values.items()
                ))

            write_cache()
            macos_cache.check(cache, ffmpeg, srt)
            values["FFmpeg_avformat_LIBRARY"] = root / "obs-deps/lib/libavformat.dylib"
            write_cache()
            with self.assertRaisesRegex(RuntimeError, "unexpected avformat"):
                macos_cache.check(cache, ffmpeg, srt)
            values["FFmpeg_avformat_LIBRARY"] = ffmpeg / "lib/libavformat.dylib"
            values["Libsrt_LIBRARY"] = root / "obs-deps/lib/libsrt.dylib"
            write_cache()
            with self.assertRaisesRegex(RuntimeError, "non-Robotweax"):
                macos_cache.check(cache, ffmpeg, srt)

    def test_macos_runtime_guard_requires_one_provider_and_decoded_media(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            srt, ffmpeg, plugin = (root / name for name in ("srt", "ffmpeg", "obs-ffmpeg.plugin"))
            log = root / "obs.log"
            provider = srt / "lib/librobotweax-srt.dylib"
            log.write_text(
                f"MAP {provider}\nMAP {ffmpeg}/lib/libavformat.dylib\n"
                f"MAP {plugin}/Contents/MacOS/obs-ffmpeg\n"
                f"BINDING native {provider}\nBINDING ffmpeg {provider}\n"
                "MEDIA video=25 changed=20 audio=30 audible=25 bytes=50000\n"
            )
            macos.require_provider(log, srt, ffmpeg, plugin)
            macos.require_media(log)
            log.write_text(log.read_text() + f"MAP {provider}\n")
            macos.require_provider(log, srt, ffmpeg, plugin)
            log.write_text(log.read_text() + f"MAP {root}/deps/libsrt.dylib\n")
            with self.assertRaisesRegex(RuntimeError, "competing SRT"):
                macos.require_provider(log, srt, ffmpeg, plugin)

    @unittest.skipUnless(
        shutil.which("cmake") and shutil.which("cc"), "requires CMake and C"
    )
    def test_macos_metal_warning_exception_is_desktop_only(self):
        build = (ROOT / "tests/obs/build_macos.sh").read_text()
        self.assertIn("-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0", build)
        self.assertIn('"${desktop_cmake[@]}"', build)
        case_block = build.split('case "$profile" in\n', 1)[1].split("\nesac", 1)[0]
        for profile, expected in (("modules", "OFF"), ("desktop", "ON")):
            result = subprocess.run(
                ["/bin/bash", "-c", "set -u\nprofile=" + profile + "\ncase $profile in\n"
                 + case_block + "\nesac\nprintf '%s' \"${desktop_cmake[@]}\""],
                check=True, capture_output=True, text=True,
            )
            self.assertIn(f"ROBOTWEAX_OBS_MACOS_DESKTOP={expected}", result.stdout)
        qualification = ROOT / "tests/obs/macos_qualification.cmake"
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            source.mkdir()
            (source / "dummy.c").write_text("int dummy(void) { return 0; }\n")
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(obs_qualification_probe C)\n"
                "add_library(obs-ffmpeg STATIC dummy.c)\n"
                "add_library(libobs-metal STATIC dummy.c)\n"
                "add_executable(obs-studio dummy.c)\n"
                f'include("{qualification}")\n'
                'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/result.txt" CONTENT '
                '"$<TARGET_PROPERTY:libobs-metal,COMPILE_WARNING_AS_ERROR>|'
                '$<TARGET_PROPERTY:libobs-metal,XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS>|'
                '$<TARGET_PROPERTY:libobs-metal,XCODE_ATTRIBUTE_SWIFT_TREAT_WARNINGS_AS_ERRORS>")\n'
            )
            deps = Path(directory) / "deps/lib"
            deps.mkdir(parents=True)
            (deps / "libmbedcrypto.dylib").write_bytes(b"pinned library fixture")
            for enabled, expected in ((False, "||"), (True, "OFF|NO|NO")):
                output = Path(directory) / ("desktop" if enabled else "modules")
                subprocess.run(
                    [
                        "cmake", "-S", str(source), "-B", str(output),
                        f"-DROBOTWEAX_OBS_MACOS_DESKTOP={'ON' if enabled else 'OFF'}",
                        f"-DROBOTWEAX_OBS_MACOS_DEPS_PREFIX={deps.parent}",
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual((output / "result.txt").read_text(), expected)

    @unittest.skipUnless(
        shutil.which("cmake") and shutil.which("cc"), "requires CMake and C"
    )
    def test_macos_desktop_embeds_pinned_librist_crypto_dependency(self):
        build = (ROOT / "tests/obs/build_macos.sh").read_text()
        self.assertIn('-DROBOTWEAX_OBS_MACOS_DEPS_PREFIX="$work/deps/obs"', build)
        qualification = ROOT / "tests/obs/macos_qualification.cmake"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            (source / "dummy.c").write_text("int main(void) { return 0; }\n")
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(obs_qualification_probe C)\n"
                "add_library(obs-ffmpeg STATIC dummy.c)\n"
                "add_library(libobs-metal STATIC dummy.c)\n"
                "add_executable(obs-studio dummy.c)\n"
                f'include("{qualification}")\n'
                'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/embedded.txt" CONTENT '
                '"$<TARGET_PROPERTY:obs-studio,XCODE_EMBED_FRAMEWORKS>")\n'
            )
            deps = root / "deps/lib"
            deps.mkdir(parents=True)
            crypto = deps / "libmbedcrypto.dylib"
            crypto.write_bytes(b"pinned library fixture")
            command = [
                "cmake", "-S", str(source), "-B", str(root / "desktop"),
                "-DROBOTWEAX_OBS_MACOS_DESKTOP=ON",
                f"-DROBOTWEAX_OBS_MACOS_DEPS_PREFIX={deps.parent}",
            ]
            subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(
                (root / "desktop/embedded.txt").read_text(), str(crypto)
            )
            crypto.unlink()
            command[command.index(str(root / "desktop"))] = str(root / "missing")
            missing = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("missing pinned OBS libmbedcrypto.dylib", missing.stderr)

    def test_macos_job_is_required_and_uploads_text_only(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        build = (ROOT / "tests/obs/build_macos.sh").read_text()
        self.assertIn('-G Xcode', build)
        self.assertIn('ENABLE_NEW_MPEGTS_OUTPUT=ON', build)
        self.assertIn('check_macos_cache.py', build)
        self.assertIn('run_macos_smoke.py', build)
        self.assertIn('--target libobs libobs-opengl obs-ffmpeg obs-x264 obs-ffmpeg-mux', build)
        self.assertNotIn('zip ', build)
        job = workflow.split("  obs_macos_integration:\n", 1)[1].split(
            "  obs_windows_desktop:\n", 1
        )[0]
        self.assertIn("runs-on: macos-26", job)
        self.assertIn("tests/obs/build_macos.sh", job)
        paths = job.split("          path: |\n", 1)[1].split(
            "          retention-days:", 1
        )[0]
        self.assertNotIn(".zip", paths)
        self.assertNotIn(".dylib", paths)
        self.assertNotIn(".plugin", paths)
        self.assertIn("      - obs_macos_integration\n", workflow)

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
            "tests/obs/build_macos.sh",
            "tests/obs/check_macos_cache.py",
            "tests/obs/macos_qualification.cmake",
            "tests/obs/macos_reference_peer.c",
            "tests/obs/run_macos_smoke.py",
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
