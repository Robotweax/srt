import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "tools/mobile_configure.py"
SPEC = importlib.util.spec_from_file_location("mobile_configure", SCRIPT)
mobile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mobile)


class MobileConfigureTests(unittest.TestCase):
    def args(self, target="android-arm64", extra=None):
        values = [target, "--build-dir", "/tmp/mobile-build",
                  "--openssl-root", "/tmp/target crypto", "--deployment-target", "28"]
        if target == "android-arm64":
            values += ["--ndk", "/tmp/android ndk"]
        return mobile.parser().parse_args(values + (extra or []))

    def test_android_explicit_dependency_and_abi(self):
        command = mobile.command(self.args())
        self.assertIn("-DANDROID_ABI=arm64-v8a", command)
        self.assertIn("-DANDROID_PLATFORM=android-28", command)
        crypto = Path("/tmp/target crypto/lib/libcrypto.a").resolve()
        self.assertIn(f"-DOPENSSL_CRYPTO_LIBRARY={crypto}", command)
        self.assertIn("-DCMAKE_POSITION_INDEPENDENT_CODE=ON", command)

    def test_ios_sdk_distinction(self):
        for target, sdk in (("ios-arm64", "iphoneos"), ("ios-simulator-arm64", "iphonesimulator")):
            command = mobile.command(self.args(target, ["--deployment-target", "15.0"]))
            self.assertIn(f"-DCMAKE_OSX_SYSROOT={sdk}", command)
            self.assertIn("-DCMAKE_SYSTEM_NAME=iOS", command)
            self.assertFalse(any("ANDROID" in part for part in command))

    def test_invalid_deployment_target(self):
        for value in ("20", "28;other", "28.0"):
            with self.assertRaises(ValueError):
                mobile.command(self.args(extra=["--deployment-target", value]))

    def test_android_requires_ndk(self):
        args = self.args()
        args.ndk = None
        with self.assertRaises(ValueError):
            mobile.command(args)

    def test_ios_rejects_ndk(self):
        with self.assertRaises(ValueError):
            mobile.command(self.args("ios-arm64", ["--ndk", "/tmp/ndk"]))

    def test_rejects_dirty_build_without_deleting(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(extra=["--build-dir", directory])
            marker = Path(directory) / "CMakeCache.txt"
            marker.touch()
            with self.assertRaisesRegex(ValueError, "must be empty"):
                mobile.preflight(args)
            self.assertTrue(marker.exists())

    def test_missing_crypto_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(extra=["--build-dir", directory, "--openssl-root", directory])
            with self.assertRaisesRegex(ValueError, "Missing target OpenSSL"):
                mobile.preflight(args)
