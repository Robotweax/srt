#!/usr/bin/env python3
"""Configure an experimental mobile build without implicit host OpenSSL discovery."""

import argparse
from pathlib import Path
import re
import shlex
import subprocess
import sys


SOURCE = Path(__file__).resolve().parents[1]


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("target", choices=("android-arm64", "ios-arm64", "ios-simulator-arm64"))
    result.add_argument("--build-dir", required=True, type=Path)
    result.add_argument("--openssl-root", required=True, type=Path,
                        help="Target OpenSSL 3 prefix containing include/ and lib/libcrypto.a")
    result.add_argument("--ndk", type=Path, help="Exact Android NDK installation")
    result.add_argument("--deployment-target", required=True,
                        help="Android API level (e.g. 28) or iOS version (e.g. 15.0); not a support claim")
    result.add_argument("--plan", action="store_true",
                        help="Print configuration without checking/installing SDKs or creating files")
    return result


def command(args):
    android = args.target == "android-arm64"
    pattern = r"[0-9]+" if android else r"[0-9]+(?:\.[0-9]+){0,2}"
    if not re.fullmatch(pattern, args.deployment_target):
        raise ValueError("Invalid deployment target")
    if android and (args.ndk is None or int(args.deployment_target) < 21):
        raise ValueError("Android ARM64 requires --ndk and API level >= 21")
    if not android and args.ndk is not None:
        raise ValueError("--ndk is only valid for Android")
    build = args.build_dir.resolve()
    if build == SOURCE or (SOURCE / ".git") == build or (SOURCE / ".git") in build.parents:
        raise ValueError("Use a separate, empty build directory")
    root = args.openssl_root.resolve()
    options = ["cmake", "-S", str(SOURCE), "-B", str(build), "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=OFF",
               "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
               "-DROBOTWEAX_SRT_BUILD_TESTS=OFF",
               "-DROBOTWEAX_SRT_BUILD_TOOLS=OFF",
               "-DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF",
               "-DROBOTWEAX_SRT_BUILD_FUZZERS=OFF",
               "-DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF",
               "-DENABLE_AEAD_API_PREVIEW=ON",
               "-DOPENSSL_USE_STATIC_LIBS=TRUE",
               f"-DOPENSSL_INCLUDE_DIR={root / 'include'}",
               f"-DOPENSSL_CRYPTO_LIBRARY={root / 'lib/libcrypto.a'}"]
    if android:
        options += [f"-DCMAKE_TOOLCHAIN_FILE={args.ndk.resolve() / 'build/cmake/android.toolchain.cmake'}",
                    "-DANDROID_ABI=arm64-v8a", f"-DANDROID_PLATFORM=android-{args.deployment_target}",
                    "-DANDROID_STL=c++_shared"]
    else:
        sdk = "iphonesimulator" if "simulator" in args.target else "iphoneos"
        options += ["-DCMAKE_SYSTEM_NAME=iOS", f"-DCMAKE_OSX_SYSROOT={sdk}",
                    "-DCMAKE_OSX_ARCHITECTURES=arm64",
                    f"-DCMAKE_OSX_DEPLOYMENT_TARGET={args.deployment_target}"]
    return options


def preflight(args):
    build = args.build_dir.resolve()
    if build.exists() and (not build.is_dir() or any(build.iterdir())):
        raise ValueError("Build directory must be empty; use a new directory (nothing is deleted)")
    root = args.openssl_root.resolve()
    for relative in ("include/openssl/opensslv.h", "lib/libcrypto.a"):
        if not (root / relative).is_file():
            raise ValueError(f"Missing target OpenSSL file: {root / relative}")
    if args.target == "android-arm64":
        if not (args.ndk / "build/cmake/android.toolchain.cmake").is_file():
            raise ValueError("Android NDK toolchain not found")
    else:
        sdk = "iphonesimulator" if "simulator" in args.target else "iphoneos"
        subprocess.run(["xcrun", "--sdk", sdk, "--show-sdk-path"], check=True)


def main():
    args = parser().parse_args()
    try:
        invocation = command(args)
        if args.plan:
            print("PLAN ONLY: SDK availability and target dependencies are not validated.")
        else:
            preflight(args)
        print(shlex.join(invocation), flush=True)
        if not args.plan:
            subprocess.run(invocation, check=True)
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Mobile configure failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
