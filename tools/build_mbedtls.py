#!/usr/bin/env python3
"""Build the immutable crypto dependency used by mbedTLS qualification."""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tarfile
import urllib.request

VERSION = "3.6.7"
SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
URL = f"https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-{VERSION}/mbedtls-{VERSION}.tar.bz2"

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--allocation-hooks',action='store_true')
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True,exist_ok=True)
    archive = root / f'mbedtls-{VERSION}.tar.bz2'
    if not archive.exists():
        urllib.request.urlretrieve(URL,archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('mbedTLS archive checksum mismatch')
    with tarfile.open(archive) as bundle:
        # The pinned archive is trusted only after verifying its exact hash.
        bundle.extractall(root)
    source = root / f'mbedtls-{VERSION}'
    if args.allocation_hooks:
        subprocess.run([sys.executable,str(source/'scripts/config.py'),'set','MBEDTLS_PLATFORM_MEMORY'],check=True)
    commands = [
        ['cmake','-S',str(source),'-B',str(root/'build'),'-DCMAKE_BUILD_TYPE=Release',
         '-DENABLE_TESTING=OFF','-DENABLE_PROGRAMS=OFF','-DUSE_SHARED_MBEDTLS_LIBRARY=OFF',
         '-DUSE_STATIC_MBEDTLS_LIBRARY=ON','-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
         f'-DCMAKE_INSTALL_PREFIX={root/"install"}'],
        ['cmake','--build',str(root/'build'),'--parallel','4'],
        ['cmake','--install',str(root/'build')],
    ]
    for command in commands:
        subprocess.run(command,check=True)
    print(f'MbedTLS_ROOT={root/"install"}')

if __name__ == '__main__':
    main()
