#!/usr/bin/env python3
"""Build identical public-API peers against clean, identified SRT sources.

No downloads, installation, sysctl changes or shared build caches. Reference
libraries stay local and must not be included in diagnostic artifacts.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import shlex
import shutil
import subprocess
from pathlib import Path

import scalability_scorecard as sc

REFERENCE_REVISION = "899348d8318eb9a3c5a5b6ec43c4a1114288773a"
ROOT = Path(__file__).resolve().parents[1]


def source_identity(path: Path, *, allow_dirty: bool = False) -> dict:
    revision = subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()
    status = subprocess.check_output(
        ["git", "-C", str(path), "status", "--porcelain"], text=True
    )
    if status and not allow_dirty:
        raise ValueError(f"source tree is dirty: {path}; use a clean worktree")
    return {"path": str(path), "revision": revision, "dirty": bool(status), "status": status}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--robotweax-source", type=Path, default=ROOT)
    parser.add_argument("--reference-source", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--linkage", choices=("static", "shared"), default="static")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--jobs", type=int, default=2, choices=range(1, 9))
    parser.add_argument("--allow-dirty-robotweax", action="store_true",
                        help="development smoke only, recorded in manifest")
    args = parser.parse_args(argv)
    if platform.system() not in ("Linux", "Darwin"):
        parser.error("this diagnostic builder currently supports Linux and macOS")
    compiler = shutil.which(args.cxx)
    if not compiler:
        parser.error("C++ compiler not found")
    sources = {
        "robotweax": args.robotweax_source.resolve(strict=True),
        "haivision": args.reference_source.resolve(strict=True),
    }
    identities = {
        name: source_identity(path, allow_dirty=(name == "robotweax" and args.allow_dirty_robotweax))
        for name, path in sources.items()
    }
    if identities["haivision"]["revision"] != REFERENCE_REVISION:
        parser.error(f"reference must be the exact Haivision 1.5.7 commit {REFERENCE_REVISION}")
    out = args.output_directory.resolve()
    out.mkdir(parents=True, exist_ok=False)
    manifest = {
        "schema_version": 1, "complete": False, "sources": identities,
        "platform": platform.platform(), "architecture": platform.machine(),
        "linkage": args.linkage, "crypto": "openssl", "commands": [],
        "harness_source": sc.program_identity(ROOT / "benchmarks/scalability_peer.cpp"),
        "harness_checkout": source_identity(ROOT, allow_dirty=True),
        "programs": {}, "libraries": {},
    }

    def run(command: list[str], label: str) -> str:
        manifest["commands"].append(command)
        sc.write_report(out / "build-manifest.json", manifest)
        with (out / f"{label}.log").open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        return (out / f"{label}.log").read_text()

    try:
        run([compiler, "--version"], "compiler")
        run(["cmake", "--version"], "cmake")
        crypto_flags = shlex.split(run(["pkg-config", "--cflags", "--libs", "libcrypto"], "crypto-flags"))
        run(["openssl", "version", "-a"], "openssl")
        shared = args.linkage == "shared"
        suffix = (".dylib" if platform.system() == "Darwin" else ".so") if shared else ".a"
        for name, source in sources.items():
            build = out / f"{name}-build"
            flags = ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_FLAGS=-g",
                     "-DCMAKE_C_FLAGS=-g", f"-DCMAKE_CXX_COMPILER={compiler}"]
            if name == "robotweax":
                flags += [f"-DBUILD_SHARED_LIBS={'ON' if shared else 'OFF'}",
                          "-DROBOTWEAX_SRT_CRYPTO_BACKEND=openssl",
                          "-DROBOTWEAX_SRT_BUILD_TESTS=OFF",
                          "-DROBOTWEAX_SRT_BUILD_TOOLS=OFF",
                          "-DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF",
                          "-DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF"]
                includes = [source / "include", build]
                library = build / f"librobotweax-srt{suffix}"
                program = out / "rwx-capacity"
            else:
                flags += [f"-DENABLE_SHARED={'ON' if shared else 'OFF'}",
                          f"-DENABLE_STATIC={'OFF' if shared else 'ON'}",
                          "-DENABLE_ENCRYPTION=ON", "-DUSE_ENCLIB=openssl",
                          "-DENABLE_BONDING=ON", "-DENABLE_APPS=OFF", "-DENABLE_TESTING=OFF"]
                includes = [source / "srtcore", build]
                library = build / f"libsrt{suffix}"
                program = out / "hvs-capacity"
            run(["cmake", "-S", str(source), "-B", str(build), *flags], f"{name}-configure")
            run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], f"{name}-build")
            shutil.copyfile(build / "CMakeCache.txt", out / f"{name}-cache.txt")
            command = [compiler, "-std=c++17", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                       str(ROOT / "benchmarks/scalability_peer.cpp"),
                       *[f"-I{path}" for path in includes], str(library), *crypto_flags, "-pthread"]
            if platform.system() == "Linux":
                command += ["-ldl"]
            if shared:
                command += [f"-Wl,-rpath,{build}"]
            run([*command, "-o", str(program)], f"{name}-peer")
            manifest["programs"][name] = sc.program_identity(program)
            manifest["libraries"][name] = sc.program_identity(library)
            loader = ["otool", "-L"] if platform.system() == "Darwin" else ["ldd"]
            run([*loader, str(program)], f"{name}-loader")
        manifest["complete"] = True
        print(json.dumps({"build_manifest": str(out / "build-manifest.json"), "complete": True}))
        return 0
    finally:
        sc.write_report(out / "build-manifest.json", manifest)


if __name__ == "__main__":
    raise SystemExit(main())
