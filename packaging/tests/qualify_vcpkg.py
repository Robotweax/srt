#!/usr/bin/env python3
"""Execute installed consumers in both configurations and installation states."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vcpkg-root', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    root = args.vcpkg_root.resolve()
    work = args.work_root.resolve()
    # Refuse reuse: the standalone phase must start without either provider.
    work.mkdir(parents=True, exist_ok=False)
    installed = work / 'installed'
    repo = Path(__file__).resolve().parents[2]
    triplet = os.environ['TRIPLET']
    runtime = os.environ.get('MSVC_RUNTIME', '')
    vcpkg = root / ('vcpkg.exe' if os.name == 'nt' else 'vcpkg')

    def run(*command: object, env: dict[str, str] | None = None) -> None:
        argv = [str(value) for value in command]
        print('+', subprocess.list2cmdline(argv), flush=True)
        subprocess.run(argv, check=True, env=env)

    def install(*ports: str) -> None:
        run(vcpkg, 'install', *(f'{port}:{triplet}' for port in ports),
            f'--overlay-ports={repo / "packaging/vcpkg/ports"}',
            f'--x-install-root={installed}', '--classic')

    def consumers(stage: str, prefix_root: Path, coexist: bool) -> None:
        for config in ('Release', 'Debug'):
            build = work / f'{stage}-{config}'
            options = [f'-DCMAKE_MSVC_RUNTIME_LIBRARY={runtime}'] if runtime else []
            run('cmake', '-S', repo / 'packaging/tests', '-B', build,
                f'-DCMAKE_BUILD_TYPE={config}',
                f'-DCMAKE_TOOLCHAIN_FILE={root / "scripts/buildsystems/vcpkg.cmake"}',
                f'-DVCPKG_INSTALLED_DIR={prefix_root}',
                f'-DVCPKG_TARGET_TRIPLET={triplet}',
                f'-DHAIVISION_PREFIX={prefix_root / triplet}',
                f'-DTEST_HAIVISION={"ON" if coexist else "OFF"}', *options)
            run('cmake', '--build', build, '--config', config)
            env = runtime_env(prefix_root, config)
            run('ctest', '--test-dir', build, '-C', config, '--output-on-failure', env=env)

    def runtime_env(prefix_root: Path, config: str) -> dict[str, str]:
        env = os.environ.copy()
        prefix = prefix_root / triplet
        binary_dir = prefix / 'debug/bin' if config == 'Debug' else prefix / 'bin'
        env['PATH'] = str(binary_dir) + os.pathsep + env.get('PATH', '')
        return env

    install('robotweax-srt')
    if (installed / triplet / 'include/srt/srt.h').exists():
        raise RuntimeError('Generic Haivision header present in standalone installation')
    consumers('standalone', installed, False)
    install('libsrt')
    consumers('coexistence', installed, True)

    # Move the entire prefix; the original path must be absent during validation.
    relocated = work / 'relocated'
    installed.rename(relocated)
    try:
        consumers('relocated', relocated, True)
    finally:
        relocated.rename(installed)

    run(vcpkg, 'remove', f'robotweax-srt:{triplet}',
        f'--x-install-root={installed}', '--classic')
    for config in ('Release', 'Debug'):
        run('ctest', '--test-dir', work / f'coexistence-{config}', '-C', config,
            '-R', '^haivision$', '--output-on-failure', env=runtime_env(installed, config))


if __name__ == '__main__':
    main()
