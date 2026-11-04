#!/usr/bin/env python3
"""Opt-in local build of the external application against an installed SDK."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PIN = '899348d8318eb9a3c5a5b6ec43c4a1114288773a'


def run(args: list[str], log: Path) -> None:
    with log.open('w') as stream:
        result = subprocess.run(args, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        diagnostic = log.read_text(errors='replace')[-12000:]
        print(f"Failed stage: {log.name}\n{diagnostic}", file=sys.stderr)
        raise subprocess.CalledProcessError(result.returncode, args)


def build(args: argparse.Namespace) -> None:
    work = args.work.resolve()
    if work == ROOT or ROOT in work.parents:
        raise RuntimeError('use a work directory outside the Robotweax source checkout')
    work.mkdir(parents=True, exist_ok=True)
    evidence = work / 'evidence'
    evidence.mkdir(exist_ok=True)
    source = args.source.resolve() if args.source else work / 'haivision-source'
    if not args.source and not source.exists():
        run(['git', '-c', 'core.autocrlf=false', 'clone', '--filter=blob:none', '--no-checkout',
             'https://github.com/Haivision/srt.git', str(source)], evidence / 'fetch.log')
        run(['git', '-C', str(source), '-c', 'core.autocrlf=false',
             'checkout', '--detach', PIN], evidence / 'checkout.log')
    sdk = args.sdk_prefix.resolve() if args.sdk_prefix else work / 'sdk'
    common = ['-DCMAKE_BUILD_TYPE=Release']
    if args.openssl_root:
        common.append(f'-DOPENSSL_ROOT_DIR={args.openssl_root.resolve()}')
    if not args.sdk_prefix:
        run(['cmake', '-S', str(ROOT), '-B', str(work / 'sdk-build'), *common,
             '-DROBOTWEAX_SRT_BUILD_TESTS=OFF', '-DROBOTWEAX_SRT_BUILD_TOOLS=OFF',
             '-DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF', '-DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF',
             f'-DCMAKE_INSTALL_PREFIX={sdk}'], evidence / 'sdk-configure.log')
        run(['cmake', '--build', str(work / 'sdk-build'), '--config', 'Release',
             '--target', 'robotweax_srt', '--parallel', str(args.jobs)], evidence / 'sdk-build.log')
        run(['cmake', '--install', str(work / 'sdk-build'), '--config', 'Release'],
            evidence / 'sdk-install.log')
    app_build = work / 'app-build'
    run(['cmake', '-S', str(ROOT / 'tests/live_transmit'), '-B', str(app_build), *common,
         f'-DHAIVISION_SRT_SOURCE_DIR={source}', f'-DCMAKE_PREFIX_PATH={sdk}',
         f'-DPython3_EXECUTABLE={sys.executable}'], evidence / 'app-configure.log')
    provider = Path((app_build / 'provider-library-Release.txt').read_text().strip()).resolve()
    if sdk not in provider.parents:
        raise RuntimeError('application resolved its transport outside the selected SDK prefix')
    run(['cmake', '--build', str(app_build), '--config', 'Release', '--parallel', str(args.jobs)],
        evidence / 'app-build.log')
    binary = Path((app_build / 'app-path-Release.txt').read_text().strip())
    # Tests produce local diagnostics only; CI uploads the JSON summaries alone.
    run([sys.executable, str(ROOT / 'tests/live_transmit/run_smoke.py'),
         '--binary', str(binary), '--artifacts', str(evidence / 'runtime')], evidence / 'smoke.log')
    identity = {'upstream_revision': PIN, 'recipe_revision': subprocess.check_output(
        ['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
        'sdk_built_from_this_checkout': not bool(args.sdk_prefix),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'provider_sha256': hashlib.sha256(provider.read_bytes()).hexdigest(),
        'provider_library_name': provider.name, 'configuration': 'Release',
        'external_transport_sources_compiled': False}
    (evidence / 'build-identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    print(f'PASS: {binary}\nEvidence: {evidence}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--source', type=Path, help='existing clean pinned Haivision checkout')
    parser.add_argument('--sdk-prefix', type=Path, help='reuse an installed SDK; skip the library build')
    parser.add_argument('--openssl-root', type=Path)
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    try:
        build(args)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'application qualification failed: {error}\n')


if __name__ == '__main__':
    main()
