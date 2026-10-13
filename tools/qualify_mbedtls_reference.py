#!/usr/bin/env python3
"""Build pinned reference binaries and use only public black-box harnesses."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

PINS = {'ctr': '899348d8318eb9a3c5a5b6ec43c4a1114288773a',
        'gcm': 'd99d2e1a3b1a213b03c7dfbea5133898935fdeea'}

def run(command, log):
    with log.open('w') as output:
        subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, check=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mbedtls-peer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[1]
    environment = os.environ.copy()
    openssl = environment.get('OPENSSL_ROOT_DIR', '')
    if openssl:
        environment['PKG_CONFIG_PATH'] = str(Path(openssl) / 'lib/pkgconfig') + ':' + environment.get('PKG_CONFIG_PATH', '')
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libcrypto'], env=environment, text=True))
    for mode, commit in PINS.items():
        source, build = root / (mode + '-source'), root / (mode + '-build')
        source.mkdir(exist_ok=True)
        subprocess.run(['git', 'init', '-q', str(source)], check=True)
        subprocess.run(['git', '-C', str(source), 'fetch', '--depth', '1', 'https://github.com/Haivision/srt.git', commit], check=True)
        subprocess.run(['git', '-C', str(source), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
        assert subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip() == commit
        configure = ['cmake', '-S', str(source), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release',
            '-DENABLE_APPS=OFF', '-DENABLE_TESTING=OFF', '-DENABLE_SHARED=OFF', '-DENABLE_STATIC=ON']
        if openssl:
            configure.append('-DOPENSSL_ROOT_DIR=' + openssl)
        if mode == 'gcm':
            configure.append('-DENABLE_AEAD_API_PREVIEW=ON')
        run(configure, root / (mode + '-configure.log'))
        run(['cmake', '--build', str(build), '--parallel', '4'], root / (mode + '-build.log'))
        peer = root / (mode + '-peer')
        compile_command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-O2',
            '-DROBOTWEAX_SRT_REFERENCE_DIAGNOSTICS=1', str(repo / 'interop/api_peer.cpp'),
            '-I' + str(source / 'srtcore'), '-I' + str(build), str(build / 'libsrt.a'),
            *flags, '-pthread', '-o', str(peer)]
        if mode == 'gcm':
            compile_command.insert(1, '-DENABLE_AEAD_API_PREVIEW=1')
        run(compile_command, root / (mode + '-peer-build.log'))
        harness = 'run_encrypted_interop.py' if mode == 'ctr' else 'run_aead_interop.py'
        run([sys.executable, str(repo / 'interop' / harness), '--robotweax-peer', str(args.mbedtls_peer.resolve()),
            '--reference-peer', str(peer)], root / (mode + '-interop.log'))
        run([sys.executable, str(repo / 'interop/run_identity_interop.py'),
            '--robotweax-peer', str(args.mbedtls_peer.resolve()), '--reference-peer', str(peer),
            '--reference-pin', commit, '--output', str(root / (mode + '-identity-evidence'))],
            root / (mode + '-identity-interop.log'))
        print(f'{mode} interoperability passed at {commit}', flush=True)

if __name__ == '__main__':
    main()
