#!/usr/bin/env python3
"""Assemble an isolated, source-identified OBS Windows preview ZIP."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile


OBS_COMMIT = "ba2f32bdf791005443988a4955e963663e16b1ed"
PACKAGE_NAME = "Robotweax-OBS-32.2.2-Windows-x64-preview"
REQUIRED_RUNTIME = {
    "bin/64bit/obs64.exe",
    "bin/64bit/obs.dll",
    "bin/64bit/srt.dll",
    "bin/64bit/platforms/qwindows.dll",
    "data/libobs/default.effect",
    "obs-plugins/64bit/obs-ffmpeg.dll",
    "obs-plugins/64bit/obs-x264.dll",
    "obs-plugins/64bit/rtmp-services.dll",
    "obs-plugins/64bit/obs-transitions.dll",
}
EXCLUDED_RUNTIME = {"bin/64bit/windows-obs-peer.exe"}
EXCLUDED_SUFFIXES = {".pdb", ".ilk", ".exp", ".lib", ".obj"}
FIXED_TIME = (2020, 1, 1, 0, 0, 0)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def require_file(path: Path) -> Path:
    if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
        raise RuntimeError(f"missing, empty, or linked package input: {path}")
    return path


def files_below(root: Path, destination: str) -> dict[str, Path]:
    if root.is_symlink() or not root.is_dir():
        raise RuntimeError(f"missing or linked package directory: {root}")
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise RuntimeError(f"linked package input: {path}")
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            result[f"{destination}/{relative}"] = require_file(path)
    if not result:
        raise RuntimeError(f"empty package directory: {root}")
    return result


def git_head(source: Path) -> str:
    return subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True, timeout=15,
    ).stdout.strip()


def collect(args) -> tuple[dict[str, Path], dict]:
    if git_head(args.obs_source) != OBS_COMMIT:
        raise RuntimeError("OBS source does not match the pinned desktop build")
    robotweax_commit = git_head(args.robotweax_source)
    runtime = {}
    for relative in ("bin/64bit", "data", "obs-plugins/64bit"):
        runtime.update(files_below(args.obs_prefix / relative, relative))
    for name in tuple(runtime):
        if name in EXCLUDED_RUNTIME or Path(name).suffix.lower() in EXCLUDED_SUFFIXES:
            del runtime[name]
    absent = REQUIRED_RUNTIME - runtime.keys()
    if absent:
        raise RuntimeError(f"OBS runtime is incomplete: {sorted(absent)}")
    srt_paths = [name for name in runtime if Path(name).name.lower() == "srt.dll"]
    if srt_paths != ["bin/64bit/srt.dll"]:
        raise RuntimeError(f"unexpected SRT providers in preview: {srt_paths}")
    if digest(runtime["bin/64bit/srt.dll"]) != digest(require_file(args.robotweax_dll)):
        raise RuntimeError("preview does not contain the selected Robotweax SRT DLL")
    if digest(runtime["bin/64bit/srt.dll"]) == digest(require_file(args.reference_srt)):
        raise RuntimeError("preview contains the reference SRT provider")

    licenses = {
        "LICENSES/OBS-COPYING": require_file(args.obs_source / "COPYING"),
        "LICENSES/Robotweax-LICENSE": require_file(args.robotweax_source / "LICENSE"),
    }
    licenses.update(files_below(args.dependency_prefix / "licenses", "LICENSES/obs-deps"))
    licenses.update(files_below(args.qt_prefix / "licenses", "LICENSES/obs-qt"))
    if not any(name.startswith("LICENSES/obs-deps/FFmpeg/") for name in licenses):
        raise RuntimeError("pinned FFmpeg license files are missing")
    if "LICENSES/obs-qt/qt6/LICENSE.GPL2" not in licenses:
        raise RuntimeError("pinned Qt license files are missing")
    for relative in ("deps", "libobs", "shared", "frontend", "plugins/obs-ffmpeg",
                     "plugins/obs-x264", "plugins/rtmp-services", "plugins/obs-transitions"):
        folder = args.obs_source / relative
        if not folder.is_dir():
            raise RuntimeError(f"missing OBS source directory: {folder}")
        for path in folder.rglob("*"):
            if path.is_symlink():
                raise RuntimeError(f"linked OBS source input: {path}")
            if path.is_file() and path.name.lower().startswith(("license", "copying")):
                name = path.relative_to(args.obs_source).as_posix()
                licenses[f"LICENSES/obs-source/{name}"] = require_file(path)

    payload = {**runtime, **licenses}
    if len(payload) != len(runtime) + len(licenses):
        raise RuntimeError("overlapping runtime and license paths")
    normalized = [name.casefold() for name in payload]
    if len(normalized) != len(set(normalized)):
        raise RuntimeError("package paths collide on Windows")
    provenance = {
        "format": 1,
        "package": PACKAGE_NAME,
        "obs_commit": OBS_COMMIT,
        "robotweax_commit": robotweax_commit,
        "dependency_release": "2026-07-15",
        "platform": "windows-x64",
        "robotweax_srt_sha256": digest(args.robotweax_dll),
    }
    return payload, provenance


def zip_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(f"{PACKAGE_NAME}/{name}", FIXED_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 0
    info.external_attr = 0x20 << 16
    return info


def verify_archive(archive: Path, manifest: dict) -> None:
    expected = {f"{PACKAGE_NAME}/{row['path']}": row for row in manifest["files"]}
    expected[f"{PACKAGE_NAME}/MANIFEST.json"] = None
    with zipfile.ZipFile(archive) as package:
        if set(package.namelist()) != set(expected):
            raise RuntimeError("preview ZIP has missing or extra entries")
        embedded = json.loads(package.read(f"{PACKAGE_NAME}/MANIFEST.json"))
        if embedded != manifest:
            raise RuntimeError("preview ZIP manifest differs from sidecar manifest")
        for name, record in expected.items():
            if record is None:
                continue
            value = hashlib.sha256()
            size = 0
            with package.open(name) as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    value.update(block)
                    size += len(block)
            if size != record["size"] or value.hexdigest() != record["sha256"]:
                raise RuntimeError(f"preview ZIP entry failed integrity check: {name}")


def build(args) -> Path:
    if args.output_dir.exists():
        raise RuntimeError("use a fresh preview output directory")
    payload, provenance = collect(args)
    notice = (
        "Robotweax OBS Windows x64 development preview\n\n"
        "This is an independently assembled, version-pinned OBS Studio build, "
        "not an official OBS Project distribution or a signed installer.\n"
        "Start bin/64bit/obs64.exe from this extracted directory. "
        "portable_mode.txt keeps its settings here; do not merge these files "
        "into another OBS installation.\n"
        "The included LICENSES directory retains source and dependency notices. "
        "Review redistribution obligations and test on physical Windows hardware "
        "before any public release.\n"
        "Source: https://github.com/obsproject/obs-studio at " + OBS_COMMIT + "\n"
        "Robotweax source: https://github.com/Robotweax/srt at "
        + provenance["robotweax_commit"] + "\n"
    ).encode()
    generated = {"portable_mode.txt": b"", "PREVIEW-README.txt": notice}
    args.output_dir.mkdir(parents=True)
    archive = args.output_dir / f"{PACKAGE_NAME}.zip"
    records = []
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9, allowZip64=True) as package:
        for name, data in sorted(generated.items()):
            package.writestr(zip_info(name), data)
            records.append({"path": name, "size": len(data),
                            "sha256": hashlib.sha256(data).hexdigest()})
        for name, path in sorted(payload.items()):
            with path.open("rb") as source, package.open(zip_info(name), "w") as target:
                shutil.copyfileobj(source, target, 1024 * 1024)
            records.append({"path": name, "size": path.stat().st_size,
                            "sha256": digest(path)})
        manifest = {**provenance, "files": sorted(records, key=lambda row: row["path"])}
        package.writestr(zip_info("MANIFEST.json"),
                         (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode())
    verify_archive(archive, manifest)
    (args.output_dir / "MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    (args.output_dir / "SHA256SUMS.txt").write_text(
        f"{digest(archive)}  {archive.name}\n"
    )
    return archive


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ("obs-prefix", "obs-source", "robotweax-source", "robotweax-dll",
                 "reference-srt", "dependency-prefix", "qt-prefix", "output-dir"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    for name, value in vars(args).items():
        setattr(args, name, value.resolve())
    print(build(args))


if __name__ == "__main__":
    main()
